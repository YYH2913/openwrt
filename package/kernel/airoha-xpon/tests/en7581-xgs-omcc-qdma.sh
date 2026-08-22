#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
patch_file="$openwrt_dir/target/linux/airoha/patches-6.18/930-net-airoha-add-xgs-omcc-consumer.patch"
driver="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgspon.c"
mac_init="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgs-mac-init.h"
eth_header="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-eth.h"
omcc_header="$openwrt_dir/package/kernel/airoha-xpon/src/airoha-xgs-omcc.h"
board_dts="$openwrt_dir/target/linux/airoha/dts/an7581-axon-xg2010g-ubi.dts"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	if ! grep -Fq -- "$pattern" "$file"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

reject_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	if grep -Fq -- "$pattern" "$file"; then
		echo "unexpected $description: $pattern" >&2
		exit 1
	fi
}

[ -f "$patch_file" ] || {
	echo "XGS OMCC QDMA patch not found at $patch_file" >&2
	exit 2
}

# The Ethernet patch must preserve every descriptor field that decides the
# trusted downstream-MIC path and must detach consumers before freeing GDMs.
require_fixed '#define QDMA_ETH_RXMSG_NO_MIC_MASK' "$patch_file" \
	'QDMA RX no-MIC descriptor bit'
require_fixed 'struct airoha_pon_rx_cb {' "$patch_file" \
	'fragment metadata record'
require_fixed 'bool no_mic;' "$patch_file" \
	'fragment no-MIC metadata'
require_fixed 'no_mic = !!(msg0 & QDMA_ETH_RXMSG_NO_MIC_MASK);' "$patch_file" \
	'first-descriptor no-MIC capture'
require_fixed 'cb->no_mic = no_mic;' "$patch_file" \
	'fragment no-MIC preservation'
require_fixed 'gem == gpon_gem;' "$patch_file" \
	'fragment XGEM consistency gate'
require_fixed 'airoha_xgs_omcc_receive(dev, q->skb, gpon_gem, no_mic)' \
	"$patch_file" 'XGS OMCC receive delivery'
require_fixed 'synchronize_net();' "$patch_file" \
	'RCU consumer detach barrier'
require_fixed 'if (valid && rcu_access_pointer(dev->xgs_omcc_consumer))' \
	"$patch_file" 'GPON and XGS OMCC mutual exclusion'

# The external XGS driver verifies frames through a bounded workqueue, exposes
# only trusted downstream records through another bounded queue and keeps TX
# disabled.
require_fixed 'struct airoha_xgs_omcc_metadata {' "$eth_header" \
	'external-module OMCC metadata ABI'
require_fixed 'airoha_xgs_omcc_register(priv->ethernet_np' "$driver" \
	'XGS Ethernet consumer registration'
require_fixed 'EN7581_XGSPON_OMCC_RX_QUEUE_LIMIT' "$driver" \
	'bounded OMCC verification queue'
require_fixed 'EN7581_XGSPON_OMCC_READY_QUEUE_LIMIT' "$driver" \
	'bounded trusted OMCC userspace queue'
require_fixed 'rx->generation = priv->session_generation;' "$driver" \
	'OMCC generation capture at ingress'
require_fixed 'static bool en7581_xgspon_omcc_session_ready' "$driver" \
	'OMCC worker removal gate'
require_fixed '!priv->removing && priv->enabled && priv->hardware_selected' "$driver" \
	'OMCC worker removal gate'
require_fixed 'if (READ_ONCE(priv->removing))' "$driver" \
	'OMCC ingress fast removal gate'
require_fixed 'else if (priv->omcc_rx_queue_depth <' "$driver" \
	'OMCC locked removal recheck before enqueue'
require_fixed 'atomic64_inc(&priv->omcc_rx_dropped_removing);' "$driver" \
	'truthful OMCC removal-drop accounting'
require_fixed 'airoha_xgs_omci_content_length(rx->skb->data' "$driver" \
	'OMCI frame boundary validation'
require_fixed 'airoha_xgs_authenticate_downstream_omci(' "$driver" \
	'software downstream OMCI MIC verification'
require_fixed '{ AIROHA_XGS_MAC_INIT_DEBUG_CAP, 0x0000011bU, 0x00000110U }' \
	"$mac_init" 'transactional downstream-only hardware OMCI MIC selection'
require_fixed 'priv->hardware_ds_omci_mic = true;' "$driver" \
	'downstream hardware MIC verified state'
require_fixed 'hardware_us_mic=0' "$driver" \
	'truthful upstream MIC evidence'
require_fixed 'AIROHA_XGS_OMCC_FLAG_MIC_VERIFIED |' "$driver" \
	'trusted downstream record flag'
require_fixed 'AIROHA_XGS_OMCC_FLAG_TRAILER_STRIPPED' "$driver" \
	'stripped downstream trailer flag'
require_fixed 'wait_event_interruptible(' "$driver" \
	'interruptible OMCC record reads'
require_fixed 'ret = -EMSGSIZE;' "$driver" \
	'atomic short-buffer rejection'
require_fixed 'copy_to_user(buffer, ready->record, ready->size)' "$driver" \
	'complete trusted record copy'
require_fixed 'atomic64_inc(&priv->omcc_rx_delivered);' "$driver" \
	'successful userspace delivery accounting'
require_fixed 'en7581_xgspon_advance_session_locked(priv);' "$driver" \
	'old-session queue purge'
require_fixed 'static ssize_t en7581_xgspon_omcc_write' "$driver" \
	'gated upstream OMCC write path'
require_fixed 'airoha_xgs_sign_upstream_omci' "$driver" \
	'kernel upstream OMCI signing'
require_fixed 'en7581_xgspon_enable_upstream_irqs_locked' "$driver" \
	'gated upstream completion/error IRQ enable'
require_fixed 'en7581_xgspon_set_mac_irq_enable(' "$driver" \
	'verified fail-closed downstream-only IRQ state'
require_fixed 'en7581_xgspon_omcc_tx_ready_locked' "$driver" \
	'OMCC TX readiness gate'
require_fixed 'tx_authorized = true' "$driver" \
	'post-registration authorization transition'
require_fixed 'airoha_en7572_set_tx_enabled(priv->bosa, true)' "$driver" \
	'optical TX enable after gated FIFO completion'

require_fixed '#define AIROHA_XGS_OMCC_CAP_DS_MIC_VERIFIED' "$omcc_header" \
	'reserved downstream-MIC capability bit'
require_fixed '#define AIROHA_XGS_OMCC_CAP_US_MIC_SIGNED' "$omcc_header" \
	'reserved upstream-MIC capability bit'
require_fixed 'ethernet = <&gdm2>;' "$board_dts" \
	'XGS OMCC QDMA consumer phandle'
require_fixed 'airoha,pon-mode = "xgspon";' "$board_dts" \
	'fail-closed XGS-PON board default mode'

echo 'EN7581 XGS OMCC QDMA receive path matches SDK metadata and remains fail-closed'
