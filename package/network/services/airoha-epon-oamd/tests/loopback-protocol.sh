#!/bin/sh
set -eu

base="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
root="$(CDPATH= cd -- "$base/../../../../.." && pwd)"
sdk="$root/../tmp/airoha-sdk/airoha_sdk/private/xpon_10g"
patch="$root/target/linux/airoha/patches-6.18/934-net-airoha-add-epon-remote-loopback.patch"
driver="$root/package/kernel/airoha-xpon/src/airoha-epon.c"
daemon="$base/../src/airoha-epon-oamd.c"
temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT INT TERM

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	grep -Fq -- "$pattern" "$file" || {
		echo "missing $description: $pattern" >&2
		exit 1
	}
}

cc -std=c11 -Wall -Wextra -Werror -I"$base/../src" \
	"$base/loopback-protocol-test.c" \
	-o "$temporary/loopback-protocol-test"
"$temporary/loopback-protocol-test"

cmp "$root/package/kernel/airoha-xpon/src/airoha-epon-oam-abi.h" \
	"$base/../src/airoha-epon-oam-abi.h"

# The vendor SDK implements remote loopback as RX_LOOPBACK plus TX_DISCARD.
sed -n '/static int xmcs_set_epon_rx_config/,/^}/p' \
	"$sdk/src/xmcs/xmcs_if.c" > "$temporary/sdk-rx"
sed -n '/static int xmcs_set_epon_tx_config/,/^}/p' \
	"$sdk/src/xmcs/xmcs_if.c" > "$temporary/sdk-tx"
require_fixed 'pRxCfg->rxMode == EPON_RX_LOOPBACK' "$temporary/sdk-rx" \
	'SDK RX loopback mode'
require_fixed 'info.rxLb = 1' "$temporary/sdk-rx" 'SDK RX loopback state'
require_fixed 'pTxCfg->txMode == EPON_TX_DISCARD' "$temporary/sdk-tx" \
	'SDK TX discard mode'

# The Linux path restores the Ethernet header, reinjects on the same LLID,
# rejects ordinary local TX, and clears state with LLID lifetime.
require_fixed 'skb_push(skb, ETH_HLEN);' "$patch" \
	'complete Ethernet frame restoration'
require_fixed 'AIROHA_EPON_LOOPBACK_MARK_KEY | llid_index' "$patch" \
	'same-LLID loopback reinjection mark'
require_fixed 'epon_mark == AIROHA_EPON_LOOPBACK_MARK_KEY' "$patch" \
	'loopback-only TX admission'
require_fixed 'dev->epon_loopback_mask &= llid_mask;' "$patch" \
	'LLID lifetime cleanup'
require_fixed 'int airoha_epon_loopback_set(' "$patch" \
	'per-LLID data-plane control'

require_fixed 'AIROHA_EPON_OAM_IOC_SET_LOOPBACK' "$driver" \
	'kernel OAM loopback ioctl'
require_fixed 'AIROHA_EPON_OAM_CONFIGURATION' "$daemon" \
	'loopback capability advertisement'
require_fixed 'return handle_loopback(state, index, frame, length);' "$daemon" \
	'IEEE loopback control dispatch'
require_fixed 'clear_loopbacks(state);' "$daemon" \
	'daemon-exit loopback cleanup'

echo 'SDK, data-plane, ABI and IEEE remote-loopback contracts verified'
