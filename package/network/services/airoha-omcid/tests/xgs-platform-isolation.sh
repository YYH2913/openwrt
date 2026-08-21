#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
package="$openwrt/package/network/services/airoha-omcid"
temporary="$(mktemp -d)"
trap 'status=$?; rm -rf "$temporary"; exit "$status"' EXIT INT TERM

mkdir -p "$temporary/gpon" "$temporary/xgspon" "$temporary/state"
printf '5 operational\n' > "$temporary/gpon/state"
printf '2:100\n' > "$temporary/gpon/tconts"
printf 'sentinel-gpon-gem-state\n' > "$temporary/gpon/data_gems"
printf 'sentinel-gpon-qos-state\n' > "$temporary/gpon/qos"
printf 'registered\n' > "$temporary/xgspon/state"
printf '1\n' > "$temporary/xgspon/activation_ready"
printf '1\n' > "$temporary/xgspon/ready"
printf 'version=1 generation=0 tconts=0 xgems=0\n' > "$temporary/xgspon/service_state"
for attribute in service_reset service_tcont service_xgem service_commit service_rollback; do
	: > "$temporary/xgspon/$attribute"
done

jq '.state_domain = "xg2010g:xgspon" |
    .mib_state.state_domain = "xg2010g:xgspon" |
    .service_graph |= with_entries(.value = []) |
    .service_graph.pon_mode = "xgspon" |
    .service_graph.tconts = [{
      entity_id: 32768, alloc_id: 16383, scheduler_policy: 2,
      scheduler_weight: 17, queue_entities: [0,0,0,0,0,0,0,0],
      queue_weights: [1,2,3,4,5,6,7,8]
    }] |
    .service_graph.gem_ports = [{
      entity_id: 32769, port_id: 50000, tcont: 32768, alloc_id: 16383,
      direction: 3, encryption_key_ring: 1
    }]' \
	"$package/tests/desired-v2.json" > "$temporary/xgspon.json"
jq '.state_domain = "xg2010g:xgpon" |
    .mib_state.state_domain = "xg2010g:xgpon" |
    .service_graph.pon_mode = "xgpon"' \
	"$temporary/xgspon.json" > "$temporary/xgpon.json"

export AIROHA_OMCI_STATE_DIR="$temporary/state"
export AIROHA_OMCI_GPON_PATH="$temporary/gpon"
export AIROHA_OMCI_XGSPON_PATH="$temporary/xgspon"
export AIROHA_OMCI_PON_MODE=xgspon
export AIROHA_OMCI_TC=/bin/true
export AIROHA_OMCI_IP=/bin/true
export AIROHA_OMCI_BRIDGE=/bin/true
export AIROHA_OMCI_MULTICAST=/bin/true
export AIROHA_OMCI_UCODE="$openwrt/staging_dir/hostpkg/bin/ucode"
export AIROHA_OMCI_RENDER_SOURCE="$package/files/airoha-omci-render.uc"
export AIROHA_OMCI_RENDER="$package/tests/render-host"
export AIROHA_OMCI_PLATFORM_SOURCE="$package/files/airoha-omci-platform"
export AIROHA_OMCI_PLATFORM="$package/tests/platform-host"

# Clearing XGS software state must never use the GPON service ABI.
"$AIROHA_OMCI_PLATFORM" clear
[ "$(cat "$temporary/gpon/data_gems")" = sentinel-gpon-gem-state ]
[ "$(cat "$temporary/gpon/qos")" = sentinel-gpon-qos-state ]

# XG-PON has a distinct persistent state domain, while sharing the staged
# 14-bit Alloc-ID/16-bit XGEM hardware ABI with XGS-PON.
export AIROHA_OMCI_PON_MODE=xgpon
"$AIROHA_OMCI_PLATFORM" apply "$temporary/xgpon.json"
[ "$(cat "$temporary/xgspon/service_tcont")" = \
	'1 16383 2 17 1 2 3 4 5 6 7 8 0 0 0 0' ]
[ "$(cat "$temporary/xgspon/service_xgem")" = '50000 1 3 1 1 0 1 1' ]
if "$AIROHA_OMCI_PLATFORM" apply "$temporary/xgspon.json"; then
	echo 'XGS-PON service graph was accepted in XG-PON mode' >&2
	exit 1
fi
[ "$(jq -r .state "$temporary/state/platform.json")" = pon-mode-mismatch ]
export AIROHA_OMCI_PON_MODE=xgspon

# The XGS staged ABI accepts the full 14-bit Alloc-ID and a 16-bit XGEM.
"$AIROHA_OMCI_PLATFORM" apply "$temporary/xgspon.json"
[ "$(jq -r .state "$temporary/state/platform.json")" = applied ]
[ "$(cat "$temporary/xgspon/service_tcont")" = \
	'1 16383 2 17 1 2 3 4 5 6 7 8 0 0 0 0' ]
[ "$(cat "$temporary/xgspon/service_xgem")" = '50000 1 3 1 1 0 1 1' ]
[ "$(cat "$temporary/xgspon/service_commit")" = commit ]
[ "$(cat "$temporary/gpon/data_gems")" = sentinel-gpon-gem-state ]
[ "$(cat "$temporary/gpon/qos")" = sentinel-gpon-qos-state ]

# If software cleanup fails after the empty XGS service commit, restore the
# previous hardware generation instead of silently leaving it disconnected.
printf 'not-a-managed-bridge\n' > "$temporary/state/platform-bridges"
if "$AIROHA_OMCI_PLATFORM" clear; then
	echo 'XGS-PON clear ignored a software cleanup failure' >&2
	exit 1
fi
[ "$(cat "$temporary/xgspon/service_rollback")" = rollback ]
[ "$(jq -r .state "$temporary/state/platform.json")" = clear-failed ]

# A failed hardware restore is a distinct fail-closed state; retaining only
# the original software error would make the active hardware state ambiguous.
printf 'not-a-managed-bridge\n' > "$temporary/state/platform-bridges"
rm -f "$temporary/xgspon/service_rollback"
ln -s /dev/full "$temporary/xgspon/service_rollback"
if "$AIROHA_OMCI_PLATFORM" clear 2>/dev/null; then
	echo 'XGS-PON clear ignored a hardware rollback failure' >&2
	exit 1
fi
[ "$(jq -r .state "$temporary/state/platform.json")" = xgs-service-rollback-failed ]
rm -f "$temporary/xgspon/service_rollback"
: > "$temporary/xgspon/service_rollback"

# Renderer range validation rejects the first values outside the XGS ranges.
jq '.service_graph.tconts[0].alloc_id = 16384' "$temporary/xgspon.json" \
	> "$temporary/bad-alloc.json"
if "$AIROHA_OMCI_RENDER" "$temporary/bad-alloc.json" >/dev/null 2>&1; then
	echo 'XGS-PON accepted Alloc-ID 16384' >&2
	exit 1
fi
jq '.service_graph.gem_ports[0].port_id = 65535' "$temporary/xgspon.json" \
	> "$temporary/bad-xgem.json"
if "$AIROHA_OMCI_RENDER" "$temporary/bad-xgem.json" >/dev/null 2>&1; then
	echo 'XGS-PON accepted XGEM 65535' >&2
	exit 1
fi

# Multicast ACLs use the same mode-specific GEM Port-ID range as service
# XGEMs. Keep the XGS 16-bit range without weakening the GPON 12-bit limit.
jq '.service_graph.multicast_operations_profiles = [{
      entity_id: 1792, igmp_version: 3, igmp_function: 0,
      immediate_leave: 1, upstream_tci: 0, upstream_tag_control: 0,
      upstream_rate: 0, robustness: 2, querier_ip_address: 0,
      query_interval: 125, query_max_response_time: 10,
      last_member_query_interval: 1, unauthorized_join_behaviour: 0,
      downstream_tag_control: 0, downstream_tci: 0, static_acl: [],
      dynamic_acl: [{
        row_key: 1, ip_version: 4, gem_port_id: 50000, vlan_id: 100,
        source: "0.0.0.0", start: "239.1.1.1", stop: "239.1.1.8",
        imputed_bandwidth: 1000000, preview_length: 0,
        preview_repeat_time: 0, preview_repeat_count: 0,
        preview_reset_time: 0
      }]
    }]' "$temporary/xgspon.json" > "$temporary/xgspon-acl.json"
"$AIROHA_OMCI_RENDER" "$temporary/xgspon-acl.json" >/dev/null
jq --slurpfile xgs "$temporary/xgspon-acl.json" \
	'.service_graph.multicast_operations_profiles =
	 $xgs[0].service_graph.multicast_operations_profiles' \
	"$package/tests/desired-v2.json" > "$temporary/gpon-high-acl.json"
if "$AIROHA_OMCI_RENDER" "$temporary/gpon-high-acl.json" >/dev/null 2>&1; then
	echo 'GPON accepted multicast ACL GEM Port-ID 50000' >&2
	exit 1
fi

# A GPON persistent document cannot be replayed while the platform is in XGS
# mode, even if a GPON driver is still present.
if "$AIROHA_OMCI_PLATFORM" apply "$package/tests/desired-v2.json"; then
	echo 'GPON service graph was accepted in XGS-PON mode' >&2
	exit 1
fi
[ "$(jq -r .state "$temporary/state/platform.json")" = pon-mode-mismatch ]
[ "$(cat "$temporary/gpon/data_gems")" = sentinel-gpon-gem-state ]
[ "$(cat "$temporary/gpon/qos")" = sentinel-gpon-qos-state ]

if "$AIROHA_OMCI_PLATFORM" apply "$temporary/xgpon.json"; then
	echo 'XG-PON service graph was accepted in XGS-PON mode' >&2
	exit 1
fi
[ "$(jq -r .state "$temporary/state/platform.json")" = pon-mode-mismatch ]

echo 'XG-PON/XGS-PON platform isolation tests passed'
