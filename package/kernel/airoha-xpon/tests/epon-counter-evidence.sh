#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)"
driver="$root/package/kernel/airoha-xpon/src/airoha-epon.c"
collector="$root/package/network/services/airoha-omcid/files/airoha-xpon-evidence"
acceptor="$root/package/network/services/luci-app-airoha-gpon/root/usr/sbin/airoha-xpon-accept-edge"
sdk_header="$root/../tmp/airoha-sdk/airoha_sdk/private/xpon_10g/inc/epon/xepon_mac_c_header_en7581.h"
sdk_source="$root/../tmp/airoha-sdk/airoha_sdk/private/xpon_10g/src/ic/AN7581.c"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	grep -Fq -- "$pattern" "$file" || {
		echo "missing $description: $pattern" >&2
		exit 1
	}
}

require_fixed 'e_rxmbi_eth_cnt;  // 6510' "$sdk_header" \
	'SDK RX MBI Ethernet frame counter'
require_fixed 'e_rxmpi_eth_cnt;  // 6514' "$sdk_header" \
	'SDK RX MPI Ethernet frame counter'
require_fixed 'e_txmbi_eth_cnt;  // 6518' "$sdk_header" \
	'SDK TX MBI Ethernet frame counter'
require_fixed 'e_txmpi_eth_cnt;  // 651C' "$sdk_header" \
	'SDK TX MPI Ethernet frame counter'
require_fixed 'e_rxmbi_bytecnt_h; // 6580' "$sdk_header" \
	'SDK RX MBI byte counter high word'
require_fixed 'e_rxmbi_bytecnt_l; // 6584' "$sdk_header" \
	'SDK RX MBI byte counter low word'
require_fixed '"e_glb_cfg2",' "$sdk_source" 'SDK global configuration 2'
require_fixed '0x6004, 0x80a08000, 0x80f3f061' "$sdk_source" \
	'SDK Ethernet-byte-count default'

require_fixed '#define EN7581_EPON_RX_MBI_ETH_COUNT' "$driver" \
	'driver RX MBI frame register'
require_fixed '#define EN7581_EPON_RX_MBI_BYTE_COUNT_HI' "$driver" \
	'driver RX byte high register'
require_fixed '#define  EN7581_EPON_ETH_COUNT_BYTES' "$driver" \
	'driver Ethernet byte-count enable'
require_fixed 'en7581_epon_pm_reset_session_locked(priv);' "$driver" \
	'per-mode EPON counter baseline'
require_fixed 'counter_reset=%u eth_byte_count_enabled=%u' "$driver" \
	'counter reset and configuration evidence'

require_fixed 'pon_counter_source=epon-mac-session' "$collector" \
	'normalized EPON counter source'
require_fixed 'received_non_idle_bytes' "$collector" \
	'normalized XG/XGS receive counter'
require_fixed 'tx_mpi_ethernet_frames' "$collector" \
	'normalized EPON transmit proof'
require_fixed 'pon_proof_met' "$acceptor" 'protocol-counter acceptance gate'
require_fixed 'after_pon_reset' "$acceptor" 'counter-reset rejection gate'

echo 'SDK-backed XG/XGS and EPON protocol traffic evidence verified'
