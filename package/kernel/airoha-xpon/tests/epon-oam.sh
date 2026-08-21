#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)"
EPON="$ROOT/package/kernel/airoha-xpon/src/airoha-epon.c"
XGS="$ROOT/package/kernel/airoha-xpon/src/airoha-xgspon.c"
HEADER="$ROOT/package/kernel/airoha-xpon/src/airoha-eth.h"
PATCH="$ROOT/target/linux/airoha/patches-6.18/933-net-airoha-add-epon-oam-consumer.patch"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	grep -Fq -- "$pattern" "$file" || {
		echo "missing $description: $pattern" >&2
		exit 1
	}
}

# The Ethernet RX path must terminate EPON OAM at its own consumer.  The
# OMCI netdev is selected only when neither XGS OMCC nor EPON OAM owns it.
require_fixed 'airoha_epon_oam_receive(dev, q->skb,' "$PATCH" \
	'dedicated EPON OAM RX delivery'
require_fixed 'if (omci && !xgs_omcc && !epon_oam)' "$PATCH" \
	'OMCI exclusion for EPON OAM'
require_fixed 'consumer->ops->receive(consumer->context, skb, &metadata);' \
	"$PATCH" 'RCU EPON OAM consumer delivery'

# Match the SDK PWAN_IF_OAM framing contract: userspace receives and sends a
# two-byte network-order LLID index followed by the complete Ethernet frame.
require_fixed 'put_unaligned_be16(metadata->llid_index, rx->data);' "$EPON" \
	'network-order RX LLID prefix'
require_fixed 'rx->data + sizeof(__be16)' "$EPON" \
	'RX frame placement after LLID prefix'
require_fixed 'metadata.llid_index = be16_to_cpu(encoded_index);' "$EPON" \
	'network-order TX LLID prefix decode'
require_fixed 'buffer + sizeof(encoded_index)' "$EPON" \
	'TX prefix removal'
require_fixed '!(priv->datapath_llid_mask & BIT(metadata.llid_index))' \
	"$EPON" 'unregistered LLID TX rejection'

# SDK epon_wan.c sends OAM with queue=7, channel=LLID index, NBOQ=LLID index
# and the descriptor OAM bit asserted.
require_fixed 'FIELD_PREP(QDMA_ETH_TXMSG_CHAN_MASK, metadata->llid_index)' \
	"$PATCH" 'EPON OAM TX channel'
require_fixed 'FIELD_PREP(QDMA_ETH_TXMSG_QUEUE_MASK,' "$PATCH" \
	'EPON OAM TX queue field'
require_fixed 'AIROHA_NUM_QOS_QUEUES - 1) |' "$PATCH" \
	'EPON OAM queue 7 selection'
require_fixed 'QDMA_ETH_TXMSG_OAM_MASK;' "$PATCH" \
	'EPON OAM descriptor bit'
require_fixed 'FIELD_PREP(QDMA_ETH_TXMSG_NBOQ_MASK, metadata->llid_index)' \
	"$PATCH" 'EPON OAM TX NBOQ'
require_fixed 'skb_len = max_t(size_t, len, ETH_ZLEN);' "$PATCH" \
	'EPON OAM minimum Ethernet frame length'
require_fixed 'skb_put_zero(skb, skb_len - len);' "$PATCH" \
	'zero-filled EPON OAM frame padding'

# EPON OAM is a separate ownership domain from GPON OMCI and XGS OMCC.
require_fixed 'rcu_access_pointer(dev->epon_oam_consumer) ||' "$PATCH" \
	'duplicate EPON consumer rejection'
require_fixed 'rcu_access_pointer(dev->xgs_omcc_consumer) || gpon_ready || xgs_active' \
	"$PATCH" 'GPON/XGS/EPON consumer exclusion'
require_fixed 'if (llid_mask && !rcu_access_pointer(dev->epon_oam_consumer))' \
	"$PATCH" 'EPON data path requires OAM owner'
require_fixed 'if (valid && rcu_access_pointer(dev->epon_oam_consumer))' \
	"$PATCH" 'GPON OMCI exclusion while EPON owns OAM'
require_fixed 'int airoha_epon_oam_register(' "$HEADER" \
	'exported EPON OAM consumer ABI'

# Consumers follow the active XPON owner transaction.  They must not remain
# registered merely because their platform drivers probed successfully.
sed -n '/static int en7581_epon_start_mac(/,/^}/p' "$EPON" > "$TMP/epon-start"
sed -n '/static int en7581_epon_stop_mac(/,/^}/p' "$EPON" > "$TMP/epon-stop"
sed -n '/static int en7581_xgspon_xpon_start_mac(/,/^}/p' "$XGS" > "$TMP/xgs-start"
sed -n '/static int en7581_xgspon_xpon_stop_mac(/,/^}/p' "$XGS" > "$TMP/xgs-stop"
require_fixed 'en7581_epon_attach_oam(priv)' "$TMP/epon-start" \
	'EPON consumer attach in start transaction'
require_fixed 'en7581_epon_detach_oam(priv)' "$TMP/epon-stop" \
	'EPON consumer detach in stop transaction'
require_fixed 'en7581_xgspon_attach_omcc(priv)' "$TMP/xgs-start" \
	'XGS consumer attach in start transaction'
require_fixed 'en7581_xgspon_detach_omcc(priv)' "$TMP/xgs-stop" \
	'XGS consumer detach in stop transaction'

require_fixed 'priv->oam.name = "airoha-epon-oam";' "$EPON" \
	'dedicated userspace EPON OAM endpoint'
require_fixed 'atomic_cmpxchg(&priv->oam_opened, 0, 1)' "$EPON" \
	'single EPON OAM userspace owner'
require_fixed 'EN7581_EPON_OAM_QUEUE_LIMIT' "$EPON" \
	'bounded EPON OAM RX queue'
require_fixed 'AIROHA_EPON_OAM_IOC_CLEAR_SESSION' "$EPON" \
	'per-LLID OAM session clear ioctl'
sed -n '/static long en7581_epon_oam_clear_session_ioctl(/,/^}/p' "$EPON" \
	> "$TMP/clear-session"
require_fixed 'airoha_epon_loopback_set(priv->ethernet_np,' \
	"$TMP/clear-session" 'remote loopback teardown on lost OAM session'
require_fixed 'en7581_epon_clear_llid_keys_locked(priv, session.llid_index)' \
	"$TMP/clear-session" 'fail-closed key removal on lost OAM session'
require_fixed 'priv->pending_ds_key_events &= ~llid_bit;' \
	"$TMP/clear-session" 'downstream key event removal for lost session'
require_fixed 'priv->pending_us_key_events &= ~llid_bit;' \
	"$TMP/clear-session" 'upstream key event removal for lost session'
require_fixed 'oam_rx=%llu oam_rx_dropped=%llu oam_tx=%llu oam_tx_errors=%llu' \
	"$EPON" 'EPON OAM statistics'

echo 'EPON OAM isolation, SDK framing and runtime ownership contracts verified'
