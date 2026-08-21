#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)"
DRIVER="$ROOT/package/kernel/airoha-xpon/src/airoha-epon.c"
PATCH="$ROOT/target/linux/airoha/patches-6.18/932-net-airoha-add-epon-llid-data-path.patch"
OAM_PATCH="$ROOT/target/linux/airoha/patches-6.18/933-net-airoha-add-epon-oam-consumer.patch"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

sed -n '/case AIROHA_EPON_REGISTER_FLAG_NACK:/,/^[[:space:]]*break;/p' \
	"$DRIVER" > "$TMP/nack"
grep -q 'en7581_epon_sync_datapath_locked(priv)' "$TMP/nack"
grep -q 'airoha_en7572_set_tx_enabled(priv->bosa, false)' "$TMP/nack"

sed -n '/static void en7581_epon_remove(/,/^}/p' "$DRIVER" > "$TMP/remove"
sed -n '/static void en7581_epon_shutdown(/,/^}/p' "$DRIVER" > "$TMP/shutdown"
grep -q 'en7581_epon_synchronize_irqs(priv)' "$TMP/remove"
grep -q 'airoha_epon_qdma_llids_clear(priv->ethernet_np)' "$TMP/remove"
grep -q 'en7581_epon_synchronize_irqs(priv)' "$TMP/shutdown"
grep -q 'airoha_epon_qdma_llids_clear(priv->ethernet_np)' "$TMP/shutdown"

grep -q 'AIROHA_EPON_MARK_KEY | cb->channel' "$PATCH"
grep -q 'hweight32(epon_mask) == 1' "$PATCH"
grep -q 'gpon_channel = __ffs(epon_mask)' "$PATCH"
grep -q 'dev->epon_llid_mask & BIT(channel)' "$PATCH"
grep -q 'dev->gpon_tx_ready || dev->gpon_omcc_ready' "$PATCH"
grep -q 'dev->xgs_service_active' "$PATCH"
grep -q 'rcu_access_pointer(dev->xgs_omcc_consumer)' "$PATCH"
grep -q 'QDMA_EPON_REPORT_FEC, report_fec_mask' "$PATCH"
grep -q 'clear_bit(channel, qdma->qos_channel_map)' "$PATCH"
grep -q 'airoha_epon_oam_receive(dev, q->skb' "$OAM_PATCH"
grep -q 'omci && !xgs_omcc && !epon_oam' "$OAM_PATCH"

echo "EPON LLID/QDMA data-path fail-closed contracts verified"
