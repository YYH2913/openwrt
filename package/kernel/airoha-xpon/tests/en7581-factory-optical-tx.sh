#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd)
CORE="$ROOT/package/kernel/airoha-xpon/src/airoha-xpon-core.c"
BOSA="$ROOT/package/kernel/airoha-xpon/src/airoha-en7572.c"
GPON="$ROOT/package/kernel/airoha-xpon/src/airoha-gpon.c"
XGSPON="$ROOT/package/kernel/airoha-xpon/src/airoha-xgspon.c"
EPON="$ROOT/package/kernel/airoha-xpon/src/airoha-epon.c"
PCS="$ROOT/target/linux/airoha/patches-6.18/622-net-pcs-airoha-timed-factory-optical-tx.patch"
PCS_BASE="$ROOT/target/linux/airoha/patches-6.18/310-09-net-pcs-airoha-add-PCS-driver-for-Airoha-AN7581-SoC.patch"
DTS="$ROOT/target/linux/airoha/dts/an7581-axon-xg2010g-ubi.dts"
SCRIPT="$ROOT/package/network/services/airoha-omcid/files/airoha-optical-tx-test"

require_fixed() {
	needle=$1
	file=$2
	description=$3
	grep -Fq "$needle" "$file" || {
		echo "missing $description" >&2
		exit 1
	}
}

require_order() {
	text=$1
	first=$2
	second=$3
	description=$4
	first_line=$(printf '%s\n' "$text" | grep -nF "$first" | head -n 1 | cut -d: -f1)
	second_line=$(printf '%s\n' "$text" | grep -nF "$second" | head -n 1 | cut -d: -f1)
	[ -n "$first_line" ] && [ -n "$second_line" ] && \
		[ "$first_line" -lt "$second_line" ] || {
		echo "invalid $description order" >&2
		exit 1
	}
}

store=$(sed -n '/^static ssize_t factory_tx_test_store(/,/^}/p' "$CORE")
disable=$(sed -n '/^static void airoha_xpon_factory_tx_disable_locked(/,/^}/p' "$CORE")
remove=$(sed -n '/^static void airoha_xpon_core_remove(/,/^}/p' "$CORE")
unregister=$(sed -n '/^void airoha_xpon_backend_unregister(/,/^}/p' "$CORE")
set_mode=$(sed -n '/^int airoha_en7572_set_mode(/,/^}/p' "$BOSA")
pcs_factory=$(awk '
	/^--- a\/drivers\/net\/pcs\/airoha\/pcs-airoha-common\.c$/ { in_common = 1 }
	in_common && /^\+int airoha_pcs_xpon_factory_tx\(/ { capture = 1 }
	capture { print }
	capture && /^\+}/ { exit }
' "$PCS")
pcs_restore=$(sed -n '/^+static int airoha_pcs_factory_tx_restore_locked(/,/^+}/p' "$PCS")
pcs_rollback=$(printf '%s\n' "$pcs_factory" | sed -n '/^+restore:/,/^+unlock:/p')
ben_enable=$(sed -n '/^static int airoha_xpon_factory_tx_ben_enable_locked(/,/^}/p' "$CORE")
ben_disable=$(sed -n '/^static int airoha_xpon_factory_tx_ben_disable_locked(/,/^}/p' "$CORE")

require_fixed 'capable(CAP_NET_ADMIN)' "$CORE" 'CAP_NET_ADMIN gate'
require_fixed 'strcmp(token, AIROHA_FACTORY_TX_TOKEN)' "$CORE" 'confirmation-token gate'
require_fixed 'duration > AIROHA_FACTORY_TX_MAX_MS' "$CORE" 'bounded duration gate'
require_fixed 'AIROHA_FACTORY_TX_MAX_MS	5000' "$CORE" 'five-second duration limit'
require_fixed 'wavelength != expected' "$CORE" 'mode/wavelength gate'
require_fixed 'core->current_backend->ops->activation_enabled' "$CORE" \
	'kernel protocol-activation gate'
require_fixed 'protocol_enabled=%u' "$CORE" \
	'kernel protocol-activation status'
require_fixed 'airoha_xpon_backend_activation_lock' "$CORE" \
	'protocol/factory transaction lock'
require_fixed 'airoha_xpon_backend_activation_lock(priv->xpon_backend)' \
	"$GPON" 'GPON activation transaction lock'
require_fixed 'airoha_xpon_backend_activation_lock(backend)' \
	"$XGSPON" 'XG/XGS-PON activation transaction lock'
require_fixed 'airoha_xpon_backend_activation_lock(backend)' \
	"$EPON" 'EPON activation transaction lock'
require_fixed 'airoha_xpon_validate_factory_tx_mode(core, core->current_mode)' "$CORE" \
	'factory TX consistency gate'
require_fixed 'airoha_xpon_validate_runtime_mode' "$CORE" \
	'full PCS/BOSA consistency gate'
require_fixed 'airoha_pcs_xpon_validate_tx_mode' "$CORE" \
	'TX-only PCS validation API'
require_fixed 'core->factory_tx_test_active)' "$CORE" 'mode-switch interlock'
require_fixed 'cancel_delayed_work(&core->factory_tx_work)' "$CORE" 'manual-stop timer cancellation'
require_fixed 'airoha,allow-factory-tx-test;' "$DTS" 'device-tree opt-in'
require_fixed 'airoha,chip-scu = <&chip_scu>;' "$DTS" 'GPON Chip SCU phandle'
require_fixed 'factory-tx-ben-gpios = <&en7581_pinctrl 41 GPIO_ACTIVE_HIGH>;' \
	"$DTS" 'factory TX BEN GPIO'
require_fixed 'XG2010G-OPTICAL-TEST off' "$SCRIPT" 'script off command'
require_fixed 'prepare_wavelength' "$SCRIPT" 'script mode preparation command'
require_fixed 'require_inactive_protocols' "$SCRIPT" 'inactive-protocol preparation gate'
require_fixed 'READ_ONCE(priv->factory_tx_test_active)' "$BOSA" 'normal-TX interlock'
require_fixed 'static DEVICE_ATTR_RO(init_status);' "$GPON" 'GPON init-status interface'
require_fixed '"airoha,chip-scu"' "$GPON" 'GPON Chip SCU lookup'
require_fixed 'GPON PHY setting readback failed' "$GPON" 'GPON PHY readback diagnostics'

require_order "$store" 'airoha_en7572_factory_tx_enable' \
	'airoha_pcs_xpon_factory_tx' 'BOSA then PCS enable'
require_order "$store" 'airoha_pcs_xpon_factory_tx' \
	'airoha_xpon_factory_tx_ben_enable_locked' 'PCS then physical BEN enable'
require_order "$disable" 'airoha_xpon_factory_tx_ben_disable_locked' \
	'airoha_pcs_xpon_factory_tx' 'BEN deassertion before PCS restore'
require_order "$disable" 'airoha_pcs_xpon_factory_tx' \
	'airoha_en7572_factory_tx_disable' 'PCS restore before BOSA disable'
require_order "$ben_enable" 'regmap_update_bits' \
	'airoha_xpon_factory_tx_ben_set(core, true)' \
	'force selection before BEN high'
require_order "$ben_disable" 'airoha_xpon_factory_tx_ben_set(core, false)' \
	'regmap_update_bits' 'BEN low before force restore'
require_fixed 'EN7581_CHIP_SCU_FORCE_GPIO32_EN' "$CORE" \
	'Chip SCU GPIO41 force register'
require_fixed 'EN7581_FACTORY_TX_BEN_FORCE' "$CORE" \
	'GPIO41 force bit'
require_fixed 'EN7581_FACTORY_TX_BEN_VALUE' "$CORE" \
	'GPIO41 forced output value bit'
require_fixed 'EN7581_FACTORY_TX_BEN_MASK' "$CORE" \
	'GPIO41 force/value mask'
require_fixed 'gpiod_get_raw_value_cansleep' "$CORE" 'factory BEN raw readback'
require_fixed 'ben_enabled=%u ben_forced=%u' "$CORE" \
	'factory BEN status'
require_fixed 'airoha_pcs_xpon_get_ben_active_high' "$PCS" \
	'PCS BEN polarity API'
require_fixed 'else if (priv->pon_quiesced)' "$PCS" \
	'PCS running-state gate after PMA recovery'
if grep -Fq 'else if (!priv->pon_pma_ready)' "$PCS"; then
	echo 'factory TX must not gate on transient pon_pma_ready' >&2
	exit 1
fi
require_fixed 'AIROHA_PCS_PMA_BURST_EN_INV' "$PCS" \
	'PCS BURST_EN_INV definition'
require_fixed 'platform_get_resource_byname' "$PCS_BASE" \
	'named-regmap resource lookup'
require_fixed 'regmap_config.max_register' "$PCS_BASE" \
	'named-regmap address limit'
require_order "$remove" 'core->removing = true' \
	'cancel_delayed_work_sync' 'remove interlock then timer cancellation'
require_order "$unregister" 'cancel_delayed_work(&core->factory_tx_work)' \
	'airoha_xpon_factory_tx_disable_locked' 'unregister timer cancellation before PCS restore'
require_fixed 'READ_ONCE(priv->factory_tx_test_active)' /dev/stdin \
	'BOSA mode-switch interlock' <<EOF
$set_mode
EOF
require_fixed 'ret = -EBUSY;' /dev/stdin 'BOSA mode-switch rejection' <<EOF
$set_mode
EOF
require_order "$pcs_factory" 'priv->factory_tx_test_active = true' \
	'regmap_clear_bits' 'saved PCS state before first force write'
require_fixed 'priv->factory_tx_saved_tx_dly_ctrl' "$PCS" \
	'factory OUTBEN state preservation'
require_fixed 'priv->factory_tx_saved_md32_clk_ctrl' "$PCS" \
	'factory MD32 clock state preservation'
require_order "$pcs_factory" 'AIROHA_PCS_PMA_TX_FORCE_CONT_MODE);' \
	'FIELD_PREP(AIROHA_PCS_PMA_OUTBEN_DATA_MODE,' \
	'continuous data before optical BEN force'
require_order "$pcs_restore" 'AIROHA_PCS_PMA_TX_DLY_CTRL' \
	'AIROHA_PCS_PMA_DA_XPON_TX_FORCE_1' \
	'optical BEN removal before continuous data restore'
require_fixed 'FIELD_PREP(AIROHA_PCS_PMA_OUTBEN_DATA_MODE,' /dev/stdin \
	'safe OUTBEN fallback' <<EOF
$pcs_restore
EOF
require_fixed 'airoha_pcs_factory_tx_restore_locked' /dev/stdin \
	'partial PCS write rollback' <<EOF
$pcs_rollback
EOF
require_fixed 'priv->factory_tx_port = port->index;' "$PCS" 'PCS port preservation'
require_fixed 'ret = restore_ret;' "$PCS" 'restore error propagation'

echo 'factory optical TX interface is bounded, mode-gated and rollback-safe'
