#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
sdk_archive="${AIROHA_SDK_ARCHIVE:-$openwrt_dir/../airoha_sdk.tar.gz}"
source_dir="$openwrt_dir/package/kernel/airoha-xpon/src"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/en7581-xpon-irq.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

[ -f "$sdk_archive" ] || {
	echo "Airoha SDK archive not found at $sdk_archive" >&2
	exit 2
}

tar -xOf "$sdk_archive" \
	airoha_sdk/private/xpon_1g/inc/gpon/gpon_mac_reg_c_header_en7521.h \
	> "$tmp_dir/gpon-mac.h"
tar -xOf "$sdk_archive" \
	airoha_sdk/private/xpon_10g/inc/gpon/xgpon_mac_reg_c_header.h \
	> "$tmp_dir/xgpon-mac.h"
tar -xOf "$sdk_archive" \
	airoha_sdk/private/xpon_10g/inc/epon/xepon_mac_c_header_en7581.h \
	> "$tmp_dir/epon-mac.h"
tar -xOf "$sdk_archive" \
	airoha_sdk/private/xpon_phy_10g/src/en7581.c \
	> "$tmp_dir/en7581-phy.c"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	if ! grep -Fq -- "$pattern" "$file"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

extract_function() {
	name="$1"
	file="$2"
	output="$3"

	sed -n "/^static .* $name(/,/^}/p" "$file" > "$output"
	[ -s "$output" ] || {
		echo "function not found: $name" >&2
		exit 1
	}
}

require_order() {
	first="$1"
	second="$2"
	file="$3"
	description="$4"
	first_line="$(grep -nF -- "$first" "$file" | head -n 1 | cut -d: -f1)"
	second_line="$(grep -nF -- "$second" "$file" | head -n 1 | cut -d: -f1)"

	if [ -z "$first_line" ] || [ -z "$second_line" ] ||
	   [ "$first_line" -ge "$second_line" ]; then
		echo "invalid order for $description" >&2
		exit 1
	fi
}

# The generated SDK register headers expose these enable registers through
# read macros; the PHY implementation also reads PCS/PMA enable state after
# writes. This establishes that driver readback is valid hardware behavior.
require_fixed '#define G_INT_ENABLE' "$tmp_dir/gpon-mac.h" \
	'SDK readable GPON interrupt-enable register'
require_fixed 'INREG32(&gpon_mac_reg_BASE->G_INT_ENABLE)' \
	"$tmp_dir/gpon-mac.h" 'SDK GPON enable read operation'
require_fixed '#define INT_ENABLE' "$tmp_dir/xgpon-mac.h" \
	'SDK readable XG/XGS-PON interrupt-enable register'
require_fixed 'INREG32(&xgpon_mac_reg_BASE->INT_ENABLE)' \
	"$tmp_dir/xgpon-mac.h" 'SDK XG/XGS enable read operation'
require_fixed 'INREG32(&XEPON_MAC_BASE->e_int_en)' "$tmp_dir/epon-mac.h" \
	'SDK EPON primary enable read operation'
require_fixed 'INREG32(&XEPON_MAC_BASE->e_int_en2)' "$tmp_dir/epon-mac.h" \
	'SDK EPON secondary enable read operation'
require_fixed 'INREG32(&XEPON_MAC_BASE->e_int_en3)' "$tmp_dir/epon-mac.h" \
	'SDK EPON LLID enable read operation'
require_fixed 'read_data = IO_GPHYREG(EN7581_XEPON_PCS_INT_EN);' \
	"$tmp_dir/en7581-phy.c" 'SDK XEPON PCS enable readback'
require_fixed 'read_data = IO_GPHYREG(EN7581_XPON_PMA_XPON_INT_EN_0);' \
	"$tmp_dir/en7581-phy.c" 'SDK PMA enable readback'

gpon="$source_dir/airoha-gpon.c"
xgs="$source_dir/airoha-xgspon.c"
epon="$source_dir/airoha-epon.c"

extract_function en7581_gpon_xpon_mask_irqs "$gpon" "$tmp_dir/gpon-mask.c"
extract_function en7581_gpon_xpon_unmask_irqs "$gpon" "$tmp_dir/gpon-unmask.c"
extract_function en7581_gpon_set_mac_irq_enable "$gpon" \
	"$tmp_dir/gpon-mac-enable.c"
extract_function en7581_gpon_hw_init "$gpon" "$tmp_dir/gpon-hw-init.c"
extract_function enabled_store "$gpon" "$tmp_dir/gpon-enabled-store.c"
extract_function en7581_xgspon_xpon_mask_irqs "$xgs" "$tmp_dir/xgs-mask.c"
extract_function en7581_xgspon_xpon_unmask_irqs "$xgs" "$tmp_dir/xgs-unmask.c"
extract_function en7581_xgspon_set_mac_irq_enable "$xgs" \
	"$tmp_dir/xgs-mac-enable.c"
extract_function en7581_xgspon_enable_upstream_irqs_locked "$xgs" \
	"$tmp_dir/xgs-upstream-enable.c"
extract_function enabled_store "$xgs" "$tmp_dir/xgs-enabled-store.c"
extract_function en7581_epon_mask_irqs "$epon" "$tmp_dir/epon-mask.c"
extract_function en7581_epon_unmask_irqs "$epon" "$tmp_dir/epon-unmask.c"

require_fixed 'WRITE_ONCE(priv->xpon_irqs_masked, true);' \
	"$tmp_dir/gpon-mask.c" 'GPON software mask gate'
require_fixed 'en7581_gpon_read(priv, EN7581_GPON_INT_ENABLE)' \
	"$tmp_dir/gpon-mask.c" 'GPON MAC mask readback'
require_fixed 'readl(priv->phy_csr + EN7581_PHY_CSR_XPON_INT_ENABLE)' \
	"$tmp_dir/gpon-mask.c" 'GPON PHY mask readback'
require_fixed 'readl(priv->pma + EN7581_PMA_XPON_INT_EN_0)' \
	"$tmp_dir/gpon-mask.c" 'GPON PMA mask readback'
require_fixed 'failed to verify GPON interrupt enable' \
	"$tmp_dir/gpon-unmask.c" 'GPON fail-closed unmask path'
require_fixed 'WRITE_ONCE(priv->xpon_irqs_masked, false);' \
	"$tmp_dir/gpon-unmask.c" 'GPON verified software unmask'
require_fixed 'en7581_gpon_read(priv, EN7581_GPON_INT_ENABLE) == mask' \
	"$tmp_dir/gpon-mac-enable.c" 'GPON exact runtime MAC IRQ readback'
require_fixed 'en7581_gpon_xpon_mask_irqs(priv);' \
	"$tmp_dir/gpon-mac-enable.c" 'GPON runtime IRQ failure mask'
require_fixed 'en7581_gpon_set_mac_irq_enable(priv, 0)' \
	"$tmp_dir/gpon-hw-init.c" 'GPON start MAC IRQ-zero verification'
require_fixed 'en7581_gpon_xpon_unmask_irqs(priv)' \
	"$tmp_dir/gpon-enabled-store.c" 'GPON direct enable verified PHY IRQ path'
require_fixed 'en7581_gpon_set_mac_irq_enable(priv,' \
	"$tmp_dir/gpon-enabled-store.c" 'GPON direct enable verified MAC IRQ path'

require_fixed 'WRITE_ONCE(priv->xpon_irqs_masked, true);' \
	"$tmp_dir/xgs-mask.c" 'XG/XGS software mask gate'
require_fixed 'readl(priv->mac + EN7581_XGSPON_INT_ENABLE)' \
	"$tmp_dir/xgs-mask.c" 'XG/XGS MAC mask readback'
require_fixed 'readl(priv->phy_csr + EN7581_XGSPON_PHY_INT_ENABLE)' \
	"$tmp_dir/xgs-mask.c" 'XG/XGS PHY mask readback'
require_fixed 'failed to verify XG/XGS-PON interrupt enable' \
	"$tmp_dir/xgs-unmask.c" 'XG/XGS fail-closed unmask path'
require_fixed 'WRITE_ONCE(priv->xpon_irqs_masked, false);' \
	"$tmp_dir/xgs-unmask.c" 'XG/XGS verified software unmask'
require_fixed 'readl(priv->mac + EN7581_XGSPON_INT_ENABLE) == mask' \
	"$tmp_dir/xgs-mac-enable.c" 'XG/XGS exact runtime MAC IRQ readback'
require_fixed 'WRITE_ONCE(priv->xpon_irqs_masked, true);' \
	"$tmp_dir/xgs-mac-enable.c" 'XG/XGS runtime IRQ failure software mask'
require_fixed 'priv->hardware_state_valid = false;' \
	"$tmp_dir/xgs-mac-enable.c" 'XG/XGS runtime IRQ failure invalidation'
require_fixed 'en7581_xgspon_set_mac_irq_enable(priv,' \
	"$tmp_dir/xgs-upstream-enable.c" \
	'XG/XGS upstream activation verified IRQ path'
require_fixed 'en7581_xgspon_set_mac_irq_enable(' \
	"$tmp_dir/xgs-enabled-store.c" 'XG/XGS direct disable verified IRQ path'

require_fixed 'WRITE_ONCE(priv->irqs_masked, true);' \
	"$tmp_dir/epon-mask.c" 'EPON software mask gate'
require_fixed 'en7581_epon_read(priv, EN7581_EPON_INT_ENABLE)' \
	"$tmp_dir/epon-mask.c" 'EPON primary mask readback'
require_fixed 'en7581_epon_read(priv, EN7581_EPON_INT_ENABLE2)' \
	"$tmp_dir/epon-mask.c" 'EPON secondary mask readback'
require_fixed 'en7581_epon_read(priv, EN7581_EPON_INT_ENABLE3)' \
	"$tmp_dir/epon-mask.c" 'EPON LLID mask readback'
require_fixed 'readl(priv->xepon_pcs + EN7581_XEPON_PCS_INT_ENABLE)' \
	"$tmp_dir/epon-mask.c" 'EPON PCS mask readback'
require_fixed 'failed to verify EPON interrupt enable' \
	"$tmp_dir/epon-unmask.c" 'EPON fail-closed unmask path'
require_fixed 'WRITE_ONCE(priv->irqs_masked, false);' \
	"$tmp_dir/epon-unmask.c" 'EPON verified software unmask'

require_order 'WRITE_ONCE(priv->xpon_irqs_masked, true);' \
	'en7581_gpon_write(priv, EN7581_GPON_INT_ENABLE, 0);' \
	"$tmp_dir/gpon-mask.c" 'GPON software gate before MMIO mask'
require_order 'WRITE_ONCE(priv->xpon_irqs_masked, true);' \
	'writel(0, priv->mac + EN7581_XGSPON_INT_ENABLE);' \
	"$tmp_dir/xgs-mask.c" 'XG/XGS software gate before MMIO mask'
require_order 'WRITE_ONCE(priv->irqs_masked, true);' \
	'en7581_epon_write(priv, EN7581_EPON_INT_ENABLE, 0);' \
	"$tmp_dir/epon-mask.c" 'EPON software gate before MMIO mask'

for driver in "$gpon" "$xgs" "$epon"; do
	require_fixed 'READ_ONCE(priv->' "$driver" \
		'IRQ handler software mask read gate'
done
