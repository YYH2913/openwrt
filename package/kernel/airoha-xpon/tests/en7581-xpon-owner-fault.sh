#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
core="$script_dir/../src/airoha-xpon-core.c"
tmp_file="$(mktemp "${TMPDIR:-/tmp}/en7581-xpon-owner-fault.XXXXXX")"
trap 'rm -f "$tmp_file"' EXIT INT TERM

sed -n '/^static void airoha_xpon_switch_fault_lock(/,/^}/p' \
	"$core" > "$tmp_file"
[ -s "$tmp_file" ] || {
	echo 'XPON rollback fault-lock callback not found' >&2
	exit 1
}

require_fixed() {
	pattern="$1"
	description="$2"

	if ! grep -Fq -- "$pattern" "$tmp_file"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

require_order() {
	first="$1"
	second="$2"
	description="$3"
	first_line="$(grep -nF -- "$first" "$tmp_file" | head -n 1 | cut -d: -f1)"
	second_line="$(grep -nF -- "$second" "$tmp_file" | head -n 1 | cut -d: -f1)"

	if [ -z "$first_line" ] || [ -z "$second_line" ] ||
	   [ "$first_line" -ge "$second_line" ]; then
		echo "invalid order for $description" >&2
		exit 1
	fi
}

require_fixed 'airoha_en7572_emergency_disable(context->core->bosa);' \
	'rollback failure optical lockout'
require_fixed 'airoha_xpon_backend_quiesce(context->target);' \
	'rollback failure target quiesce'
require_fixed 'context->previous->context != context->target->context' \
	'shared XG/XGS backend deduplication'
require_fixed 'airoha_xpon_backend_quiesce(context->previous);' \
	'rollback failure previous quiesce'
require_fixed 'airoha_pcs_xpon_quiesce(context->core->pcs);' \
	'rollback failure PCS quiesce'
require_fixed 'WRITE_ONCE(context->core->current_backend, NULL);' \
	'rollback failure owner clear'
require_fixed 'context->core->current_mode = AIROHA_XPON_MODE_INVALID;' \
	'rollback failure invalid mode publication'
require_order 'airoha_en7572_emergency_disable(context->core->bosa);' \
	'airoha_xpon_backend_quiesce(context->target);' \
	'optical lockout before backend cleanup'
require_order 'airoha_pcs_xpon_quiesce(context->core->pcs);' \
	'WRITE_ONCE(context->core->current_backend, NULL);' \
	'PCS quiesce before owner clear'

echo 'XPON rollback failure clears owner after fail-closed hardware quiesce'
