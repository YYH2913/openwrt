#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
source_dir="$openwrt_dir/package/kernel/airoha-xpon/src"
board_dts="$openwrt_dir/target/linux/airoha/dts/an7581-axon-xg2010g-ubi.dts"
gpon="$source_dir/airoha-gpon.c"
xgs="$source_dir/airoha-xgspon.c"
epon="$source_dir/airoha-epon.c"
core="$source_dir/airoha-xpon-core.c"
claim="$source_dir/airoha-xpon-claim-transaction.h"
rollback="$source_dir/airoha-xpon-rollback-transaction.h"
transaction="$source_dir/airoha-xpon-switch-transaction.h"
pcs_validation_patch="$openwrt_dir/target/linux/airoha/patches-6.18/614-net-pcs-airoha-validate-runtime-xpon-mode.patch"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	grep -Fq -- "$pattern" "$file" || {
		echo "missing $description: $pattern" >&2
		exit 1
	}
}

require_fixed 'xpon_controller: xpon-controller {' "$board_dts" \
	'XPON runtime owner node'
[ "$(grep -Fc 'airoha,xpon-controller = <&xpon_controller>;' "$board_dts")" -eq 3 ] || {
	echo 'GPON, XG/XGS-PON and EPON must reference the same XPON runtime owner' >&2
	exit 1
}
require_fixed 'AIROHA_XPON_MODE_GPON' "$gpon" 'GPON backend registration'
require_fixed 'AIROHA_XPON_MODE_XGPON' "$xgs" 'XG-PON backend registration'
require_fixed 'AIROHA_XPON_MODE_XGSPON' "$xgs" 'XGS-PON backend registration'
require_fixed 'AIROHA_XPON_MODE_EPON_10G_1G' "$epon" \
	'10G/1G EPON backend registration'
require_fixed 'AIROHA_XPON_MODE_EPON_10G_10G' "$epon" \
	'10G/10G EPON backend registration'
require_fixed 'IRQF_ONESHOT | IRQF_SHARED' "$gpon" 'GPON shared IRQ request'
require_fixed 'IRQF_ONESHOT | IRQF_SHARED' "$xgs" 'XGS-PON shared IRQ request'
require_fixed 'IRQF_ONESHOT | IRQF_SHARED' "$epon" 'EPON shared IRQ request'
require_fixed 'airoha_xpon_backend_is_active(priv->xpon_backend)' "$gpon" \
	'GPON active-backend IRQ gate'
require_fixed 'airoha_xpon_backend_is_active(priv->xgpon_backend)' "$xgs" \
	'XG-PON active-backend IRQ gate'
require_fixed 'airoha_xpon_backend_is_active(priv->xgspon_backend)' "$xgs" \
	'XGS-PON active-backend IRQ gate'
require_fixed 'airoha_xpon_backend_is_active(priv->asymmetric_backend)' "$epon" \
	'10G/1G EPON active-backend IRQ gate'
require_fixed 'airoha_xpon_backend_is_active(priv->symmetric_backend)' "$epon" \
	'10G/10G EPON active-backend IRQ gate'

if grep -Eq '\<(disable_irq|disable_irq_nosync|enable_irq)\>' "$gpon" "$xgs" "$epon"; then
	echo 'a protocol backend must not disable or enable the shared XPON IRQ line' >&2
	exit 1
fi

require_fixed 'target = airoha_xpon_find_backend(core, mode);' "$core" \
	'registered-backend mode gate'
require_fixed 'if (!target || !target->ready)' "$core" \
	'unimplemented-mode rejection'
require_fixed 'airoha_xpon_core_claim_backend(target)' "$core" \
	'same-mode owner recovery'
require_fixed 'int airoha_xpon_backend_ready(' "$core" \
	'post-probe backend readiness gate'
require_fixed '.suppress_bind_attrs = true' "$core" \
	'core manual-unbind lifetime protection'
require_fixed 'airoha_xpon_backend_ready(priv->xpon_backend)' "$gpon" \
	'GPON post-probe owner claim'
require_fixed 'airoha_xpon_backend_ready(priv->xgpon_backend)' "$xgs" \
	'XG-PON post-endpoint owner claim'
require_fixed 'airoha_xpon_backend_ready(priv->xgspon_backend)' "$xgs" \
	'XGS-PON post-endpoint owner claim'
require_fixed 'airoha_xpon_backend_ready(priv->asymmetric_backend)' "$epon" \
	'10G/1G EPON post-endpoint owner claim'
require_fixed 'airoha_xpon_backend_ready(priv->symmetric_backend)' "$epon" \
	'10G/10G EPON post-endpoint owner claim'
require_fixed 'airoha_pcs_xpon_select_wan(context->core->pcs,' "$core" \
	'hardware-backed WAN-selection transaction stage'
require_fixed 'airoha_xpon_prepare_initial_mode(core, core->current_mode);' \
	"$core" 'boot-time PCS mode transaction'
require_fixed 'airoha_pcs_xpon_validate_mode(core->pcs, pcs_mode);' "$core" \
	'PCS hardware proof before owner publication'
require_fixed 'return airoha_en7572_validate_mode(core->bosa, mode);' "$core" \
	'BOSA live TX-eye hardware proof'
require_fixed 'int airoha_pcs_xpon_validate_mode(struct phylink_pcs *pcs,' \
	"$pcs_validation_patch" 'exported PCS runtime proof'
require_fixed 'airoha_en7572_tx_is_disabled(core->bosa)' "$core" \
	'TX_DISABLE transaction verification'
require_fixed 'airoha_xpon_switch_target_start_attempted(failed_stage)' "$core" \
	'stage-aware target rollback gate'
require_fixed 'return airoha_xpon_backend_quiesce(context->target);' "$core" \
	'target session cleanup after partial startup'
require_fixed 'WRITE_ONCE(context->core->current_backend, context->target);' \
	"$core" 'target owner commit before IRQ unmask'
require_fixed 'ops->commit_owner(context);' "$claim" \
	'backend owner commit before IRQ unmask'
require_fixed 'ops->clear_owner(context);' "$claim" \
	'failed backend claim owner cleanup'
require_fixed 'failed_stage=%u' "$core" \
	'observable failed transaction stage'
require_fixed 'return en7581_epon_stop_path(priv);' "$epon" \
	'EPON datapath stop without overwriting the saved enable state'

initial_mode_body="$(sed -n \
	'/^static int airoha_xpon_prepare_initial_mode(/,/^}/p' "$core")"
printf '%s\n' "$initial_mode_body" | awk '
	/airoha_pcs_xpon_quiesce\(core->pcs\)/ && step == 0 { step = 1 }
	/airoha_pcs_xpon_select_wan\(core->pcs, pcs_mode\)/ && step == 1 { step = 2 }
	/airoha_pcs_xpon_set_mode\(core->pcs, pcs_mode\)/ && step == 2 { step = 3 }
	/airoha_pcs_xpon_recover\(core->pcs\)/ && step == 3 { step = 4 }
	/airoha_xpon_validate_runtime_mode\(core, mode\)/ && step == 4 { step = 5 }
	END { exit step == 5 ? 0 : 1 }
' || {
	echo 'initial PON mode must run the complete PCS transaction before validation' >&2
	exit 1
}

[ "$(grep -Fc 'return en7581_epon_stop_path(priv);' "$epon")" -eq 2 ] || {
	echo 'EPON block and stop callbacks must share only the path-stop primitive' >&2
	exit 1
}

stop_datapath_body="$(sed -n \
	'/static int en7581_epon_stop_datapath/,/^}/p' "$epon")"
if printf '%s\n' "$stop_datapath_body" | grep -Fq 'en7581_epon_block_traffic'; then
	echo 'EPON stop_datapath must not overwrite switch_resume_enabled' >&2
	exit 1
fi

require_fixed 'cancel_delayed_work_sync(&priv->activation_work);' "$gpon" \
	'GPON rollback worker cancellation'
require_fixed 'cancel_delayed_work_sync(&priv->key_tk5_work);' "$xgs" \
	'XG/XGS rollback worker cancellation'

awk '
	/backend->ops->mask_irqs\(/ && step == 0 { step = 1 }
	/backend->ops->synchronize_irqs\(/ && step == 1 { step = 2 }
	/backend->ops->stop_datapath\(/ && step == 2 { step = 3 }
	/backend->ops->clear_session\(/ && step == 3 { step = 4 }
	/backend->ops->stop_mac\(/ && step == 4 { step = 5 }
	END { exit step == 5 ? 0 : 1 }
' "$core" || {
	echo 'target rollback must mask, synchronize, stop traffic, clear the session, then stop the MAC' >&2
	exit 1
}
require_fixed 'goto previous_failed;' "$rollback" \
	'fail-closed cleanup after partial previous-backend restore'
require_fixed 'airoha_xpon_rollback_transaction(&airoha_xpon_core_rollback_ops,' \
	"$core" 'production rollback transaction wiring'
require_fixed 'WRITE_ONCE(context->core->current_backend, context->previous);' \
	"$core" 'logical owner restoration before IRQ unmask'

claim_body="$(sed -n \
	'/^static int airoha_xpon_core_claim_backend(/,/^}/p' "$core")"
printf '%s\n' "$claim_body" | awk '
	/airoha_xpon_validate_runtime_mode\(core, backend->mode\)/ && step == 0 { step = 1 }
	/airoha_xpon_claim_transaction\(/ && step == 1 { step = 2 }
	END { exit step == 2 ? 0 : 1 }
' || {
	echo 'initial backend claim must validate live PCS and BOSA before starting the claim transaction' >&2
	exit 1
}

forward_recover_body="$(sed -n \
	'/^static int airoha_xpon_switch_recover_pcs(/,/^}/p' "$core")"
printf '%s\n' "$forward_recover_body" | grep -Fq \
	'context->target->mode' || {
	echo 'forward PCS recovery must validate the target hardware mode' >&2
	exit 1
}

rollback_calibration_body="$(sed -n \
	'/^static int airoha_xpon_rollback_load_previous_calibration(/,/^}/p' "$core")"
printf '%s\n' "$rollback_calibration_body" | grep -Fq \
	'airoha_xpon_validate_bosa_mode(context->core, mode)' || {
	echo 'rollback calibration must verify BOSA readiness and previous mode' >&2
	exit 1
}

rollback_recover_body="$(sed -n \
	'/^static int airoha_xpon_rollback_recover_pcs(/,/^}/p' "$core")"
printf '%s\n' "$rollback_recover_body" | grep -Fq \
	'context->previous->mode' || {
	echo 'rollback PCS recovery must validate the previous hardware mode' >&2
	exit 1
}

switch_validate_body="$(sed -n \
	'/^static int airoha_xpon_switch_validate_mode(/,/^}/p' "$core")"
printf '%s\n' "$switch_validate_body" | grep -Fq \
	'airoha_xpon_validate_runtime_mode(context->core, mode)' || {
	echo 'target MAC/data-path start must be followed by final hardware validation' >&2
	exit 1
}

claim_validate_body="$(sed -n \
	'/^static int airoha_xpon_claim_validate_mode(/,/^}/p' "$core")"
printf '%s\n' "$claim_validate_body" | grep -Fq \
	'airoha_xpon_validate_runtime_mode(backend->core, backend->mode)' || {
	echo 'initial claim must revalidate hardware after backend start' >&2
	exit 1
}

rollback_validate_body="$(sed -n \
	'/^static int airoha_xpon_rollback_validate_previous(/,/^}/p' "$core")"
printf '%s\n' "$rollback_validate_body" | grep -Fq \
	'airoha_xpon_validate_runtime_mode(context->core, mode)' || {
	echo 'rollback must revalidate restored hardware after backend start' >&2
	exit 1
}

awk '
	/ops->validate_mode\(context, target_mode\)/ && step == 0 { step = 1 }
	/ops->commit_owner\(context, target_mode\)/ && step == 1 { step = 2 }
	/ops->unmask_irqs\(context, target_mode\)/ && step == 2 { step = 3 }
	/ops->activate_mode\(context, target_mode\)/ && step == 3 { step = 4 }
	END { exit step == 4 ? 0 : 1 }
' "$transaction" || {
	echo 'target hardware must validate before owner commit, IRQ unmask and activation' >&2
	exit 1
}

awk '
	/ops->validate_mode\(context\)/ && step == 0 { step = 1 }
	/ops->commit_owner\(context\)/ && step == 1 { step = 2 }
	/ops->unmask_irqs\(context\)/ && step == 2 { step = 3 }
	/ops->activate_mode\(context\)/ && step == 3 { step = 4 }
	END { exit step == 4 ? 0 : 1 }
' "$claim" || {
	echo 'claim hardware must validate before owner commit, IRQ unmask and activation' >&2
	exit 1
}

awk '
	/ops->validate_previous\(context, previous_mode\)/ && step == 0 { step = 1 }
	/ops->commit_previous_owner\(context, previous_mode\)/ && step == 1 { step = 2 }
	/ops->unmask_previous_irqs\(context\)/ && step == 2 { step = 3 }
	/ops->resume_previous\(context\)/ && step == 3 { step = 4 }
	END { exit step == 4 ? 0 : 1 }
' "$rollback" || {
	echo 'rollback hardware must validate before owner restore, IRQ unmask and resume' >&2
	exit 1
}

echo 'EN7581 GPON/XG-PON/XGS-PON/EPON runtime owner and shared-IRQ contract verified'
