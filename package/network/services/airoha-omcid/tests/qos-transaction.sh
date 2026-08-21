#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
package="$openwrt/package/network/services/airoha-omcid"
temporary="$(mktemp -d)"
trap 'status=$?; chmod 0644 "$temporary/gpon/data_gems" 2>/dev/null || true; rm -rf "$temporary"; exit "$status"' EXIT INT TERM

mkdir -p "$temporary/gpon" "$temporary/state"
export AIROHA_OMCI_STATE_DIR="$temporary/state"
export AIROHA_OMCI_GPON_PATH="$temporary/gpon"
export AIROHA_OMCI_PON_DEVICE=pon
export AIROHA_OMCI_IP=/bin/true
export AIROHA_OMCI_TC="$package/tests/tc-log"
export AIROHA_OMCI_TC_LOG="$temporary/tc.log"
export AIROHA_OMCI_BRIDGE=/bin/false
export AIROHA_OMCI_UCODE="$openwrt/staging_dir/hostpkg/bin/ucode"
export AIROHA_OMCI_RENDER_SOURCE="$package/files/airoha-omci-render.uc"
export AIROHA_OMCI_RENDER="$package/tests/render-host"
export AIROHA_OMCI_PLATFORM_SOURCE="$package/files/airoha-omci-platform"
export AIROHA_OMCI_PLATFORM="$package/tests/platform-host"

printf '5 operational\n' > "$temporary/gpon/state"

reset_driver() {
	printf '5 operational\n' > "$temporary/gpon/state"
	printf '0:100 9:101\n' > "$temporary/gpon/tconts"
	printf 'off\n' > "$temporary/gpon/data_gems"
	rm -rf "$temporary/gpon/qos"
	printf 'off\n' > "$temporary/gpon/qos"
}

make_document() {
	local output="$1"
	jq -n '
{
  version: 7,
  state_domain: "xg2010g:gpon",
  operation: "set",
  mib_data_sync: 1,
  mib_state: {
    version: 1,
    state_domain: "xg2010g:gpon",
    mib_data_sync: 1,
    instances: [{
      class_id: 2,
      entity_id: 0,
      origin: 0,
      attributes: [{name: "MibDataSync", kind: "uint8", unsigned: 1}]
    }]
  },
  service_graph: {
    unis: [
      {entity_id: 257, interface: "lan1", administrative_state: 0,
       operational_state: 0, configuration: 4}
    ],
    tconts: [
      {entity_id: 32769, alloc_id: 100, scheduler_policy: 2, scheduler_weight: 17,
       queue_entities: [1,2,3,4,5,6,7,8], queue_weights: [1,2,3,4,5,6,7,8]},
      {entity_id: 32770, alloc_id: 101, scheduler_policy: 1, scheduler_weight: 0,
       queue_entities: [9,10,11,12,13,14,15,16], queue_weights: [8,7,6,5,4,3,2,1]}
    ],
    traffic_descriptors: [
      {entity_id: 34816, cir: 100000, pir: 200000, cbs: 64, pbs: 128,
       colour_mode: 0, ingress_colour_marking: 0, egress_colour_marking: 0, meter_type: 0}
    ],
    dot1_rate_limiters: [],
    gem_ports: [
      {entity_id: 1024, port_id: 200, tcont: 32769, alloc_id: 100, direction: 3,
       upstream_traffic_descriptor: 34816, downstream_traffic_descriptor: 65535},
      {entity_id: 1025, port_id: 201, tcont: 32769, alloc_id: 100, direction: 3,
       upstream_traffic_descriptor: 34816, downstream_traffic_descriptor: 65535},
      {entity_id: 1026, port_id: 202, tcont: 32770, alloc_id: 101, direction: 3,
       upstream_traffic_descriptor: 65535, downstream_traffic_descriptor: 65535}
    ],
    gem_interworking: [
      {entity_id: 2048, gem_port: 1024, option: 5, service_profile: 2304}
    ],
    pbit_mappers: [
      {entity_id: 2304, tp_type: 1, tp_pointer: 257,
       pbits: [2048,2048,2048,2048,2048,2048,2048,2048],
       unmarked_frame_option: 1, default_pbit: 0,
       dscp_to_pbit: [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]}
    ],
    bridges: [], vlan_filters: [],
    vlan_operations: [], extended_vlans: []
  }
}' > "$output"
}

expect_failure() {
	if "$AIROHA_OMCI_PLATFORM" apply "$1"; then
		echo "unexpected success applying $1" >&2
		exit 1
	fi
}

state() {
	jq -r .state "$temporary/state/platform.json"
}

reset_driver
make_document "$temporary/base.json"

# Two GEMs share one T-CONT and one meter; the second T-CONT uses SP.
"$AIROHA_OMCI_PLATFORM" apply "$temporary/base.json"
[ "$(cat "$temporary/gpon/data_gems")" = '200:0:3 201:0:3 202:9:3' ]
[ "$(cat "$temporary/gpon/qos")" = '0:2:1:2:3:4:5:6:7:8:100000:200000:64:128 9:1:8:7:6:5:4:3:2:1:0:0:0:0' ]
[ "$(state)" = applied ]

# An empty graph must release both data GEMs and native QoS.
jq '.service_graph = {unis: [], tconts: [], traffic_descriptors: [], dot1_rate_limiters: [], gem_ports: [],
  gem_interworking: [], pbit_mappers: [], bridges: [], vlan_filters: [],
  vlan_operations: [], extended_vlans: []}' "$temporary/base.json" > "$temporary/empty.json"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/empty.json"
[ "$(cat "$temporary/gpon/data_gems")" = off ]
[ "$(cat "$temporary/gpon/qos")" = off ]

reset_driver
"$AIROHA_OMCI_PLATFORM" apply "$temporary/base.json"

# Unsupported native meter features and inconsistent shared-T-CONT profiles
# must be rejected before either sysfs table is changed.
jq '.service_graph.traffic_descriptors += [{entity_id: 34817, cir: 1, pir: 2, cbs: 1, pbs: 1}] |
    .service_graph.gem_ports[1].upstream_traffic_descriptor = 34817' \
	"$temporary/base.json" > "$temporary/conflict.json"
expect_failure "$temporary/conflict.json"
[ "$(state)" = invalid-graph ]
[ "$(cat "$temporary/gpon/data_gems")" = '200:0:3 201:0:3 202:9:3' ]
[ "$(cat "$temporary/gpon/qos")" = '0:2:1:2:3:4:5:6:7:8:100000:200000:64:128 9:1:8:7:6:5:4:3:2:1:0:0:0:0' ]

jq '.service_graph.gem_ports[0].downstream_traffic_descriptor = 34816' \
	"$temporary/base.json" > "$temporary/downstream-td.json"
"$AIROHA_OMCI_RENDER" "$temporary/downstream-td.json" > "$temporary/downstream-td.rendered"
grep -qx 'down-meter 200 200000 128' "$temporary/downstream-td.rendered"
: > "$AIROHA_OMCI_TC_LOG"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/downstream-td.json"
[ "$(state)" = applied ]
grep -Fqx 'filter replace dev pon ingress chain 2010 protocol all pref 1200 handle 0xa17000c8/0xffffffff fw action police rate 1600000bit burst 128b conform-exceed drop/pipe action mirred egress redirect dev lan1' \
	"$AIROHA_OMCI_TC_LOG"
: > "$AIROHA_OMCI_TC_LOG"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/base.json"
if grep -q 'action police' "$AIROHA_OMCI_TC_LOG"; then
	echo 'removed downstream descriptor left a police action' >&2
	exit 1
fi

# A zero PBS selects the documented XG2010G factory burst. A downstream
# descriptor on an upstream-only GEM is structurally invalid.
jq '.service_graph.gem_ports[0].downstream_traffic_descriptor = 34816 |
    .service_graph.traffic_descriptors[0].pbs = 0' \
	"$temporary/base.json" > "$temporary/downstream-factory-burst.json"
"$AIROHA_OMCI_RENDER" "$temporary/downstream-factory-burst.json" \
	> "$temporary/downstream-factory-burst.rendered"
grep -qx 'down-meter 200 200000 2000' "$temporary/downstream-factory-burst.rendered"
grep -q '100000:200000:64:2000' "$temporary/downstream-factory-burst.rendered"

jq '.service_graph.gem_ports[0].downstream_traffic_descriptor = 34816 |
    .service_graph.gem_ports[0].direction = 2' \
	"$temporary/base.json" > "$temporary/downstream-wrong-direction.json"
expect_failure "$temporary/downstream-wrong-direction.json"
[ "$(state)" = invalid-graph ]

# Class-47 inbound/outbound descriptors resolve to ingress/egress policers on
# the UNI and on the profile-specific bridge ANI endpoint.
jq '.service_graph.pbit_mappers[0].tp_type = 0 |
    .service_graph.pbit_mappers[0].tp_pointer = 65535 |
    .service_graph.bridges = [{
      entity_id: 256, spanning_tree: 0, learning: 1, port_bridging: 0,
      priority: 32768, max_age_256ths: 5120, hello_time_256ths: 512,
      forward_delay_256ths: 3840, unknown_mac_discard: 0,
      mac_learning_depth: 0, dynamic_filtering_age_time_seconds: 300,
      ports: [{
        entity_id: 1280, port: 1, tp_type: 1, tp: 257, priority: 128,
        path_cost: 10, spanning_tree: 0, outbound_td: 34816,
        inbound_td: 34816, mac_learning_depth: 0
      }, {
        entity_id: 1281, port: 2, tp_type: 3, tp: 2304, priority: 128,
        path_cost: 10, spanning_tree: 0, outbound_td: 34816,
        inbound_td: 34816, mac_learning_depth: 0
      }]
    }]' "$temporary/base.json" > "$temporary/bridge-port-td.json"
"$AIROHA_OMCI_RENDER" "$temporary/bridge-port-td.json" > "$temporary/bridge-port-td.rendered"
grep -qx 'port-meter lan1 egress 200000:128' "$temporary/bridge-port-td.rendered"
grep -qx 'port-meter lan1 ingress 200000:128' "$temporary/bridge-port-td.rendered"
grep -qx 'port-meter oma0100 egress 200000:128' "$temporary/bridge-port-td.rendered"
grep -qx 'port-meter oma0100 ingress 200000:128' "$temporary/bridge-port-td.rendered"

jq '.service_graph.bridges[0].ports[0].outbound_td = 34817' \
	"$temporary/bridge-port-td.json" > "$temporary/bridge-port-missing-td.json"
if "$AIROHA_OMCI_RENDER" "$temporary/bridge-port-missing-td.json" >/dev/null 2>&1; then
	echo 'MAC bridge port with a missing traffic descriptor was accepted' >&2
	exit 1
fi

jq '.service_graph.traffic_descriptors[0].pbs = 0' \
	"$temporary/bridge-port-td.json" > "$temporary/bridge-port-factory-burst.json"
"$AIROHA_OMCI_RENDER" "$temporary/bridge-port-factory-burst.json" \
	> "$temporary/bridge-port-factory-burst.rendered"
grep -qx 'port-meter lan1 egress 200000:2000' \
	"$temporary/bridge-port-factory-burst.rendered"

# A direct mapper with class-298 policy is routed through a private two-port
# bridge so unknown unicast is selected by a real FDB miss, not by treating all
# unicast as unknown.
jq '.service_graph.dot1_rate_limiters = [{
      entity_id: 35072, parent_me: 2304, tp_type: 2,
      upstream_unicast_flood_traffic_descriptor: 34816,
      upstream_broadcast_traffic_descriptor: 65535,
      upstream_multicast_payload_traffic_descriptor: 65535
    }]' "$temporary/base.json" > "$temporary/direct-mapper-rate-limit.json"
"$AIROHA_OMCI_RENDER" "$temporary/direct-mapper-rate-limit.json" \
	> "$temporary/direct-mapper-rate-limit.rendered"
grep -qx 'bridge omm0900 2304 0:1:0:32768:2000:200:1500:0:30000:0' \
	"$temporary/direct-mapper-rate-limit.rendered"
grep -qx 'bridge-ani omm0900 omx0900 omy0900' \
	"$temporary/direct-mapper-rate-limit.rendered"
grep -qx 'rate-limit omx0900 unknown 200000:128' \
	"$temporary/direct-mapper-rate-limit.rendered"
grep -qx 'down 200 omy0900 0' "$temporary/direct-mapper-rate-limit.rendered"
if grep -qx 'down 200 lan1 0' "$temporary/direct-mapper-rate-limit.rendered"; then
	echo 'direct mapper limiter bypasses its FDB endpoint downstream' >&2
	exit 1
fi

export AIROHA_OMCI_IP="$package/tests/ip-log"
export AIROHA_OMCI_IP_LOG="$temporary/ip.log"
export AIROHA_OMCI_BRIDGE=/bin/true
: > "$AIROHA_OMCI_IP_LOG"
: > "$AIROHA_OMCI_TC_LOG"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/direct-mapper-rate-limit.json"
[ "$(state)" = applied ]
grep -Fqx 'link add name omm0900 type bridge stp_state 0 priority 32768 max_age 2000 hello_time 200 forward_delay 1500 ageing_time 30000 mcast_snooping 0' \
	"$AIROHA_OMCI_IP_LOG"
grep -Fqx 'link add name omx0900 type veth peer name omy0900' "$AIROHA_OMCI_IP_LOG"
grep -Fqx 'link set dev lan1 master omm0900' "$AIROHA_OMCI_IP_LOG"
grep -Fqx 'link set dev omx0900 master omm0900' "$AIROHA_OMCI_IP_LOG"
grep -Fqx 'filter replace dev omy0900 ingress protocol all pref 1 flower skip_hw action mirred egress redirect dev pon' \
	"$AIROHA_OMCI_TC_LOG"
grep -Fqx 'filter replace dev omx0900 egress protocol all pref 1700 flower skip_hw l2_miss 1 dst_mac 00:00:00:00:00:00/01:00:00:00:00:00 action police rate 1600000bit burst 128b conform-exceed drop/pipe' \
	"$AIROHA_OMCI_TC_LOG"
grep -Fqx 'filter replace dev pon ingress chain 2010 protocol all pref 1200 handle 0xa17000c8/0xffffffff fw action mirred egress redirect dev omy0900' \
	"$AIROHA_OMCI_TC_LOG"
grep -Fqx 'filter replace dev lan1 ingress chain 2010 protocol all pref 400 flower skip_hw action skbedit mark 0xa17000c8 action pass' \
	"$AIROHA_OMCI_TC_LOG"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/base.json"
[ ! -s "$temporary/state/platform-bridges" ]
export AIROHA_OMCI_IP=/bin/true
export AIROHA_OMCI_BRIDGE=/bin/false

jq '.service_graph.traffic_descriptors[0].colour_mode = 1' \
	"$temporary/base.json" > "$temporary/colour.json"
expect_failure "$temporary/colour.json"
[ "$(state)" = invalid-graph ]

jq '.service_graph.tconts[0].queue_weights = [0,0,0,0,0,0,0,0]' \
	"$temporary/base.json" > "$temporary/zero-wrr.json"
expect_failure "$temporary/zero-wrr.json"
[ "$(state)" = invalid-graph ]

# O5 and Alloc-ID mappings are prerequisites for touching native QoS.
printf '4 standby\n' > "$temporary/gpon/state"
expect_failure "$temporary/base.json"
[ "$(state)" = waiting-o5 ]
printf '5 operational\n' > "$temporary/gpon/state"
printf '0:100\n' > "$temporary/gpon/tconts"
expect_failure "$temporary/base.json"
[ "$(state)" = waiting-tcont ]
printf '0:100 9:101\n' > "$temporary/gpon/tconts"

# Missing qos ABI is reported without changing GEM state.
rm -f "$temporary/gpon/qos"
expect_failure "$temporary/base.json"
[ "$(state)" = qos-unavailable ]
[ "$(cat "$temporary/gpon/data_gems")" = '200:0:3 201:0:3 202:9:3' ]
printf 'off\n' > "$temporary/gpon/qos"

# A failed QoS write is reported before data GEMs are touched.
rm -rf "$temporary/gpon/qos"
mkdir "$temporary/gpon/qos"
expect_failure "$temporary/base.json"
[ "$(state)" = qos-apply-failed ]
[ "$(cat "$temporary/gpon/data_gems")" = '200:0:3 201:0:3 202:9:3' ]
rm -rf "$temporary/gpon/qos"
printf 'off\n' > "$temporary/gpon/qos"

# If the GEM table write fails after QoS succeeds, the old QoS text is put
# back. A read-only temporary file provides a deterministic write failure.
printf 'old-qos\n' > "$temporary/gpon/qos"
printf 'off\n' > "$temporary/gpon/data_gems"
chmod 0444 "$temporary/gpon/data_gems"
expect_failure "$temporary/base.json"
[ "$(cat "$temporary/gpon/qos")" = old-qos ]
chmod 0644 "$temporary/gpon/data_gems"

echo 'qos transaction tests passed'
