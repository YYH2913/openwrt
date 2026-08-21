#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)"
driver="$root/package/kernel/airoha-xpon/src/airoha-epon.c"
abi="$root/package/kernel/airoha-xpon/src/airoha-epon-oam-abi.h"
daemon_abi="$root/package/network/services/airoha-epon-oamd/src/airoha-epon-oam-abi.h"
ctc="$root/package/network/services/airoha-epon-oamd/src/airoha-epon-ctc.h"
dpoe="$root/package/network/services/airoha-epon-oamd/src/airoha-epon-dpoe.h"
sdk="$root/../tmp/airoha-sdk/airoha_sdk/private/xpon_10g/src/ic/AN7581.c"
sdk_ioctl="$root/../tmp/airoha-sdk/airoha_sdk/private/xpon_10g/src/epon/epon_ioctl.c"
stock_oam="$root/../tmp/stock-rootfs-a/userfs/bin/epon_oam"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	grep -Fq -- "$pattern" "$file" || {
		echo "missing $description: $pattern" >&2
		exit 1
	}
}

# The stock aStdFECmode entry is followed by branch 0x0007 and leaf 0x013a.
[ "$(od -An -tx1 -j $((0x39fd0)) -N4 "$stock_oam" | tr -d ' \n')" = \
	"07003a01" ]
require_fixed '#define AIROHA_CTC_STANDARD_FEC_MODE 0x013a' "$ctc" \
	'CTC standard FEC-mode leaf'
require_fixed '#define AIROHA_CTC_STANDARD_FEC_ENABLED 2' "$ctc" \
	'CTC enabled wire value'
require_fixed '#define AIROHA_CTC_STANDARD_FEC_DISABLED 3' "$ctc" \
	'CTC disabled wire value'

# The stock table entry starts at aExtFecMode (0x3cf98); its descriptor is
# 0x58 bytes later.  The preceding 0xd7/0x0604 descriptor belongs to QueueCIR.
[ "$(od -An -tx1 -j $((0x3cff0)) -N4 "$stock_oam" | tr -d ' \n')" = \
	"d7000506" ]
require_fixed '#define AIROHA_DPOE_SIA_EXTENDED_FEC_MODE 0x0605' "$dpoe" \
	'DPoE extended FEC-mode leaf'
require_fixed '#define AIROHA_DPOE_SIA_EXTENDED_FEC_LENGTH 2' "$dpoe" \
	'DPoE RX/TX FEC value width'

# AN7581 updates the LLID MAC FEC bit and the QDMA report-FEC mask together.
require_fixed 'int an7581_epon_set_llid_tx_fec(const void *in, void *out)' \
	"$sdk" 'SDK LLID TX-FEC setter'
require_fixed 'feFec |= (1<<llidIndex);' "$sdk" 'SDK QDMA FEC enable'
require_fixed 'feFec &= ~(1<<llidIndex);' "$sdk" 'SDK QDMA FEC disable'
require_fixed '10G_10G EPON mode should set QDMA[%x] DBA Report FEC OFF.' \
	"$sdk" 'SDK 10G/10G report-FEC prohibition'
require_fixed 'int eponSetLlidRxFec(__u8 llidIndex, __u8 fecFlag)' \
	"$sdk_ioctl" 'SDK LLID RX-FEC state setter'
require_fixed 'rx_fec_flag = fecFlag;' "$sdk_ioctl" \
	'SDK LLID RX-FEC state update'

cmp "$abi" "$daemon_abi"
require_fixed 'AIROHA_EPON_OAM_IOC_GET_FEC' "$abi" 'FEC get ioctl'
require_fixed 'AIROHA_EPON_OAM_IOC_SET_FEC' "$abi" 'FEC set ioctl'
require_fixed 'priv->active_mode == AIROHA_XPON_MODE_EPON_10G_10G &&' \
	"$driver" '10G/10G FEC mode guard'
require_fixed 'ret = en7581_epon_sync_datapath_locked(priv);' "$driver" \
	'transactional QDMA report-FEC update'
require_fixed 'en7581_epon_set_llid_tx_fec(priv, fec.llid_index, previous_tx);' \
	"$driver" 'MAC FEC rollback'
require_fixed 'priv->llids[fec.llid_index].rx_fec = previous_rx;' \
	"$driver" 'RX-FEC state rollback'

echo 'AN7581 EPON LLID FEC ABI, CTC wire encoding and mode constraints verified'
