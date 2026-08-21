#!/bin/sh

set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
driver="$openwrt/package/kernel/airoha-xpon/src/airoha-gpon.c"
collector="$openwrt/package/network/services/airoha-omcid/files/airoha-xpon-evidence"
acceptor="$openwrt/package/network/services/luci-app-airoha-gpon/root/usr/sbin/airoha-xpon-accept-edge"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	grep -Fq -- "$pattern" "$file" || {
		echo "missing $description: $pattern" >&2
		exit 1
	}
}

require_fixed 'static ssize_t counter_evidence_show' "$driver" \
	'read-only GPON aggregate counter'
require_fixed 'for_each_set_bit(gem, priv->data_gems' "$driver" \
	'data-GEM-only aggregation'
require_fixed 'EN7581_GPON_GEM_RX_FRAMES' "$driver" 'GPON RX frame counter'
require_fixed 'EN7581_GPON_GEM_RX_PAYLOAD_BYTES' "$driver" \
	'GPON RX payload counter'
require_fixed 'EN7581_GPON_GEM_TX_FRAMES' "$driver" 'GPON TX frame counter'
require_fixed 'EN7581_GPON_GEM_TX_PAYLOAD_BYTES' "$driver" \
	'GPON TX payload counter'
require_fixed '&dev_attr_counter_evidence.attr' "$driver" \
	'GPON counter sysfs publication'
require_fixed 'pon_counter_source=gpon-gem-data-payload' "$collector" \
	'GPON collector counter semantics'
require_fixed 'rx_payload_bytes' "$collector" 'GPON collector RX evidence'
require_fixed 'tx_payload_bytes' "$collector" 'GPON collector TX evidence'
require_fixed "case \"\$state\" in '5 O5-operation')" "$acceptor" \
	'GPON O5 acceptance gate'
require_fixed 'gpon_safety_status' "$acceptor" 'GPON safety acceptance gate'
require_fixed 'gpon_optical_link' "$acceptor" 'GPON optical acceptance gate'

echo 'GPON read-only data counter evidence contract passed'
