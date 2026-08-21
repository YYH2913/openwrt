#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
driver="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-gpon.c"
transaction="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-gpon-port-transaction.h"
test_source="$script_dir/gpon-port-transaction-test.c"
binary="$(mktemp "${TMPDIR:-/tmp}/gpon-port-transaction.XXXXXX")"
trap 'status=$?; rm -f "$binary"; exit "$status"' EXIT INT TERM

require_fixed() {
	pattern="$1"
	description="$2"

	if ! grep -Fq -- "$pattern" "$driver"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

# The production handlers must use the shared executable transaction core and
# connect every callback to the real hardware or upstream PLOAM boundary.
require_fixed '#include "airoha-gpon-port-transaction.h"' \
	'Port-ID transaction core include'
require_fixed 'return en7581_gpon_set_gem(transaction->priv, port, valid, encrypted);' \
	'GEM hardware transaction adapter'
require_fixed 'return airoha_gpon_set_omcc(transaction->priv->ethernet_np, port, valid);' \
	'Ethernet OMCC transaction adapter'
require_fixed 'en7581_gpon_write(priv, EN7581_GPON_OMCI_ID,' \
	'committed OMCC hardware state'
require_fixed 'return en7581_gpon_send_ack(transaction->priv, transaction->message);' \
	'upstream ACK transaction adapter'
require_fixed 'en7581_gpon_reset_session(transaction->priv, EN7581_GPON_STATE_O1);' \
	'rollback-failure fail-closed adapter'
require_fixed 'ret = airoha_gpon_session_reset_transaction(' \
	'session cleanup transaction call'
require_fixed 'ret = en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O7);' \
	'Disable Serial Number complete session cleanup'
require_fixed 'ret = airoha_en7572_clear_fault(priv->bosa);' \
	'Disable Serial Number Allow retry unlock'
require_fixed 'if (priv->enabled && !priv->olt_disabled && priv->phy_ready)' \
	'OLT-disable automatic reactivation gate'
require_fixed 'if (en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O7))' \
	'rogue-ONU complete session cleanup'
require_fixed 'IRQF_ONESHOT | IRQF_SHARED' \
	'shared GPON/XGS-PON IRQ request'
require_fixed 'airoha_xpon_backend_is_active(priv->xpon_backend)' \
	'active-backend IRQ dispatch gate'
require_fixed 'synchronize_irq(priv->mac_irq);' \
	'MAC IRQ transaction synchronization'
require_fixed 'synchronize_irq(priv->phy_irq);' \
	'PHY IRQ transaction synchronization'
if grep -Eq '\<(disable_irq|disable_irq_nosync|enable_irq)\>' "$driver"; then
	echo 'GPON must not enable or disable the shared XPON IRQ line' >&2
	exit 1
fi
require_fixed 'en7581_gpon_reset_session(priv, EN7581_GPON_STATE_O1);' \
	'driver lifecycle complete session cleanup'
require_fixed 'airoha_gpon_configure_port_transaction(' \
	'Configure-Port-ID shared transaction call'
require_fixed 'airoha_gpon_encrypted_port_transaction(' \
	'Encrypted-Port-ID shared transaction call'
require_fixed 'if (ret)' 'failed transaction accounting gate'

cc -std=c11 -Wall -Wextra -Werror -pedantic \
	-I"$(dirname "$transaction")" "$test_source" -o "$binary"
"$binary"

echo 'EN7581 GPON PLOAM Port-ID transactions are fault-injected and fail closed'
