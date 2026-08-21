#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
package="$openwrt/package/network/services/airoha-omcid"
temporary="$(mktemp -d)"
trap 'status=$?; rm -rf "$temporary"; exit "$status"' EXIT
trap 'exit 1' INT TERM
mkdir -p "$temporary/gpon" "$temporary/state"
printf '5 operational\n' > "$temporary/gpon/state"
printf '2:100\n' > "$temporary/gpon/tconts"
printf 'off\n' > "$temporary/gpon/data_gems"

ip link add pon type veth peer name olt
ip link add lan1 type veth peer name cpe
ip link add lan2 type veth peer name cpe2
ip link set lo up
ip link add vrf-cpe type vrf table 10
ip link add vrf-olt type vrf table 20
ip link add vrf-cpe2 type vrf table 30
ip link set vrf-cpe up
ip link set vrf-olt up
ip link set vrf-cpe2 up
ip link set cpe master vrf-cpe
ip link set cpe2 master vrf-cpe2
ip link set olt master vrf-olt
ip link set pon up
ip link set olt up
ip link set lan1 up
ip link set lan2 up
ip link set cpe up
ip link set cpe2 up

export AIROHA_OMCI_STATE_DIR="$temporary/state"
export AIROHA_OMCI_ONU3_STATE="$temporary/persistent/onu3-state.json"
export AIROHA_OMCI_GPON_PATH="$temporary/gpon"
export AIROHA_OMCI_PON_DEVICE=pon
export AIROHA_OMCI_TC_HOST="${AIROHA_OMCI_TC_HOST:-${AIROHA_OMCI_TC:-/usr/sbin/tc}}"
export AIROHA_OMCI_TC="$AIROHA_OMCI_TC_HOST"
export AIROHA_OMCI_UCODE="$openwrt/staging_dir/hostpkg/bin/ucode"
export AIROHA_OMCI_RENDER_SOURCE="$package/files/airoha-omci-render.uc"
export AIROHA_OMCI_RENDER="$package/tests/render-host"
export AIROHA_OMCI_PLATFORM_SOURCE="$package/files/airoha-omci-platform"
export AIROHA_OMCI_PLATFORM="$package/tests/platform-host"
export AIROHA_OMCI_MULTICAST=/bin/true

# Use the injected native backend for both platform operations and test
# readback. Older host tc binaries cannot parse newer flower keys such as
# l2_miss even when the running kernel accepted the rule.
tc() {
	"$AIROHA_OMCI_TC_HOST" "$@"
}

# A single-tag replacement can copy PCP, TPID and DEI from the received inner
# tag while assigning a new VID. Exact filter values are resolved without a
# packet-dependent native backend.
jq '.service_graph.extended_vlans[0].enhanced_mode = 1 |
    .service_graph.extended_vlans[0].rules[0].direction = 1 |
    .service_graph.extended_vlans[0].rules[0].filter_inner =
      {priority: 5, vid: 10, tpid_dei: 7} |
    .service_graph.extended_vlans[0].rules[0].tags_to_remove = 1 |
    .service_graph.extended_vlans[0].rules[0].treatment_inner =
      {priority: 8, vid: 100, tpid_dei: 0}' \
	"$package/tests/desired-v2.json" > "$temporary/evto-copy.json"
"$AIROHA_OMCI_RENDER" "$temporary/evto-copy.json" > "$temporary/evto-copy.rendered"
awk '$1 == "evto-up" {
       split($3, f, ":");
       ok = f[3] == 1 && f[4] == 5 && f[5] == 10 && f[6] == 33024 &&
            f[19] == 5 && f[20] == 100 && f[21] == 33024 && f[22] == 1
     }
     END { exit !ok }' "$temporary/evto-copy.rendered"

# TPID/DEI code 7 uses the configured output TPID and explicitly sets DEI.
jq '.service_graph.extended_vlans[0].enhanced_mode = 1 |
    .service_graph.extended_vlans[0].rules[0].direction = 1 |
    .service_graph.extended_vlans[0].rules[0].treatment_inner.tpid_dei = 7' \
	"$package/tests/desired-v2.json" > "$temporary/evto-dei-treatment.json"
"$AIROHA_OMCI_RENDER" "$temporary/evto-dei-treatment.json" \
	> "$temporary/evto-dei-treatment.rendered"
awk '$1 == "evto-up" {
       split($3, f, ":");
       ok = f[21] == 33024 && f[22] == 1
     }
     END { exit !ok }' "$temporary/evto-dei-treatment.rendered"

# Modes 4 and 7 match and reverse only PCP. Their inverse treatment preserves
# the downstream VID; the unmatched-frame policy differs between the modes.
jq '.service_graph.extended_vlans[0].downstream_mode = 4 |
    .service_graph.extended_vlans[0].rules[0].filter_inner =
      {priority: 5, vid: 10, tpid_dei: 4} |
    .service_graph.extended_vlans[0].rules[0].tags_to_remove = 1 |
    .service_graph.extended_vlans[0].rules[0].treatment_inner =
      {priority: 6, vid: 100, tpid_dei: 4}' \
	"$package/tests/desired-v2.json" > "$temporary/evto-pbit-inverse.json"
"$AIROHA_OMCI_RENDER" "$temporary/evto-pbit-inverse.json" \
	> "$temporary/evto-mode4.rendered"
awk '$1 == "evto-down" {
       split($3, f, ":");
       ok = f[3] == 1 && f[4] == 6 && f[5] == -1 &&
            f[13] == 1 && f[14] == 1 && f[19] == 5 && f[20] == -1
     }
     $1 == "evto-down-policy" && $3 == "pass" { policy = 1 }
     END { exit !(ok && policy) }' "$temporary/evto-mode4.rendered"
jq '.service_graph.extended_vlans[0].downstream_mode = 7' \
	"$temporary/evto-pbit-inverse.json" > "$temporary/evto-mode7.json"
"$AIROHA_OMCI_RENDER" "$temporary/evto-mode7.json" > "$temporary/evto-mode7.rendered"
awk '$1 == "evto-down" {
       split($3, f, ":");
       ok = f[3] == 1 && f[4] == 6 && f[5] == -1 &&
            f[13] == 1 && f[14] == 1 && f[19] == 5 && f[20] == -1
     }
     $1 == "evto-down-policy" && $3 == "drop" { policy = 1 }
     END { exit !(ok && policy) }' "$temporary/evto-mode7.rendered"

# Inserting an outer tag without removing the received tag produces a
# two-tag downstream match whose inverse only pops the inserted tag.
jq '.service_graph.extended_vlans[0].enhanced_mode = 1 |
    .service_graph.extended_vlans[0].downstream_mode = 3 |
    .service_graph.extended_vlans[0].rules[0].filter_inner =
      {priority: 5, vid: 10, tpid_dei: 4} |
    .service_graph.extended_vlans[0].rules[0].tags_to_remove = 0 |
    .service_graph.extended_vlans[0].rules[0].treatment_inner =
      {priority: 6, vid: 100, tpid_dei: 4}' \
	"$package/tests/desired-v2.json" > "$temporary/evto-insert.json"
"$AIROHA_OMCI_RENDER" "$temporary/evto-insert.json" > "$temporary/evto-insert.rendered"
awk '$1 == "evto-down" {
       split($3, f, ":");
       ok = f[3] == 2 && f[4] == -1 && f[5] == 100 &&
            f[7] == -1 && f[8] == 10 && f[13] == 1 && f[14] == 0
     }
     END { exit !ok }' "$temporary/evto-insert.rendered"
export AIROHA_OMCI_TC_DEI_LOG="$temporary/tc-inverse-dei.log"
export AIROHA_OMCI_TC_TRANSFORM_LOG="$temporary/tc-transform.log"
export AIROHA_OMCI_TC="$package/tests/tc-host-dei"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/evto-insert.json"
tc -j filter show dev lan1 egress chain 2011 pref 10 |
	jq -e '[.[] | .options.actions[]? | select(.kind == "vlan") | .vlan_action] == ["pop"]' \
		>/dev/null

# G.988 allows two tags to be inserted outside a double-tagged frame. Linux
# flower records the exact four-tag depth; the inverse follows the G.988 rule
# for unavailable inner information and removes the two visible treatment tags.
jq '.service_graph.extended_vlans[0].downstream_mode = 3 |
    .service_graph.extended_vlans[0].rules[0].filter_outer =
      {priority: 4, vid: 20, tpid_dei: 4} |
    .service_graph.extended_vlans[0].rules[0].treatment_outer =
      {priority: 7, vid: 200, tpid_dei: 4}' \
	"$temporary/evto-insert.json" > "$temporary/evto-four-tag.json"
"$AIROHA_OMCI_RENDER" "$temporary/evto-four-tag.json" > "$temporary/evto-four-tag.rendered"
awk '$1 == "evto-down" {
       split($3, f, ":");
       ok = f[3] == 4 && f[4] == -1 && f[5] == 200 &&
            f[7] == -1 && f[8] == -1 && f[13] == 2 && f[14] == 0
     }
     END { exit !ok }' "$temporary/evto-four-tag.rendered"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/evto-four-tag.json"
tc -j filter show dev lan1 egress chain 2011 pref 10 |
	jq -e '[.[] | .options.actions[]? | select(.kind == "vlan") | .vlan_action] ==
               ["pop", "pop"]' >/dev/null

# Replacing one tag while inserting another is inverted atomically. The
# restored tag copies its packet-dependent P-bit from the original inner
# downstream tag even though the outer treatment tag is removed.
jq '.service_graph.extended_vlans[0].rules[0].tags_to_remove = 1 |
    .service_graph.extended_vlans[0].rules[0].treatment_outer =
      {priority: 7, vid: 200, tpid_dei: 4}' \
	"$temporary/evto-insert.json" > "$temporary/evto-modify-insert.json"
"$AIROHA_OMCI_RENDER" "$temporary/evto-modify-insert.json" \
	> "$temporary/evto-modify-insert.rendered"
awk '$1 == "evto-down" {
       split($3, f, ":");
       ok = f[3] == 2 && f[5] == 200 && f[8] == 100 &&
            f[13] == 2 && f[14] == 1 && f[19] == -1 && f[20] == 10
     }
     END { exit !ok }' "$temporary/evto-modify-insert.rendered"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/evto-modify-insert.json"
grep -q 'vlan transform pop_tags 2 push_tags 1 outer .*priority inner' \
	"$AIROHA_OMCI_TC_TRANSFORM_LOG"

# A fixed double-tag replacement is exactly reversible with pop, modify and
# push. The innermost pop/push pair is deliberately folded into MODIFY.
jq '.service_graph.extended_vlans[0].downstream_mode = 2 |
    .service_graph.extended_vlans[0].rules[0].filter_outer =
      {priority: 4, vid: 20, tpid_dei: 4} |
    .service_graph.extended_vlans[0].rules[0].filter_inner =
      {priority: 5, vid: 10, tpid_dei: 4} |
    .service_graph.extended_vlans[0].rules[0].tags_to_remove = 2 |
    .service_graph.extended_vlans[0].rules[0].treatment_outer =
      {priority: 7, vid: 200, tpid_dei: 4} |
    .service_graph.extended_vlans[0].rules[0].treatment_inner =
      {priority: 6, vid: 100, tpid_dei: 4}' \
	"$package/tests/desired-v2.json" > "$temporary/evto-double-replace.json"
"$AIROHA_OMCI_RENDER" "$temporary/evto-double-replace.json" \
	> "$temporary/evto-double-replace.rendered"
awk '$1 == "evto-down" {
       split($3, f, ":");
       ok = f[3] == 2 && f[4] == 7 && f[5] == 200 &&
            f[7] == 6 && f[8] == 100 && f[13] == 2 && f[14] == 2 &&
            f[15] == 4 && f[16] == 20 && f[19] == 5 && f[20] == 10
     }
     END { exit !ok }' "$temporary/evto-double-replace.rendered"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/evto-double-replace.json"
tc -j filter show dev lan1 egress chain 2011 pref 10 |
	jq -e '[.[] | .options.actions[]? | select(.kind == "vlan") | .vlan_action] ==
               ["pop", "modify", "push"]' >/dev/null
for mode in 3 6; do
	jq ".service_graph.extended_vlans[0].downstream_mode = $mode" \
		"$temporary/evto-double-replace.json" > "$temporary/evto-double-partial.json"
	"$AIROHA_OMCI_RENDER" "$temporary/evto-double-partial.json" \
		> "$temporary/evto-double-partial.rendered"
	awk '$1 == "evto-down" {
	       split($3, f, ":");
	       ok = f[29] == "outer" && f[32] == "outer" &&
	            f[33] == "inner" && f[36] == "inner"
	     }
	     END { exit !ok }' "$temporary/evto-double-partial.rendered"
	"$AIROHA_OMCI_PLATFORM" apply "$temporary/evto-double-partial.json"
done
grep -q 'vlan transform pop_tags 2 push_tags 2 outer .*priority outer.*dei outer inner .*priority inner.*dei inner' \
	"$AIROHA_OMCI_TC_TRANSFORM_LOG"
for mode in 4 7; do
	jq ".service_graph.extended_vlans[0].downstream_mode = $mode" \
		"$temporary/evto-double-replace.json" > "$temporary/evto-double-partial.json"
	"$AIROHA_OMCI_RENDER" "$temporary/evto-double-partial.json" \
		> "$temporary/evto-double-partial.rendered"
	awk '$1 == "evto-down" {
	       split($3, f, ":");
	       ok = f[30] == "outer" && f[32] == "outer" &&
	            f[34] == "inner" && f[36] == "inner"
	     }
	     END { exit !ok }' "$temporary/evto-double-partial.rendered"
	"$AIROHA_OMCI_PLATFORM" apply "$temporary/evto-double-partial.json"
done
grep -q 'vlan transform pop_tags 2 push_tags 2 outer .*id outer.*dei outer inner .*id inner.*dei inner' \
	"$AIROHA_OMCI_TC_TRANSFORM_LOG"

# A genuinely pushed tag can copy a wildcard PCP from the received single tag.
# G.988 calls that tag inner; the kernel snapshot correctly calls it outer.
jq '.service_graph.extended_vlans[0].enhanced_mode = 1 |
    .service_graph.extended_vlans[0].rules[0].direction = 1 |
    .service_graph.extended_vlans[0].rules[0].filter_inner =
      {priority: 8, vid: 4096, tpid_dei: 0} |
    .service_graph.extended_vlans[0].rules[0].tags_to_remove = 0 |
    .service_graph.extended_vlans[0].rules[0].treatment_inner =
      {priority: 8, vid: 4096, tpid_dei: 0}' \
	"$package/tests/desired-v2.json" > "$temporary/evto-wildcard-copy.json"
"$AIROHA_OMCI_RENDER" "$temporary/evto-wildcard-copy.json" \
	> "$temporary/evto-wildcard-copy.rendered"
awk '$1 == "evto-up" {
       split($3, f, ":");
       ok = f[33] == "outer" && f[34] == "outer" &&
            f[35] == "outer" && f[36] == "outer"
     }
     END { exit !ok }' "$temporary/evto-wildcard-copy.rendered"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/evto-wildcard-copy.json"
grep -q 'vlan transform pop_tags 0 push_tags 1 outer protocol outer id outer priority outer dei outer' \
	"$AIROHA_OMCI_TC_TRANSFORM_LOG"

# The atomic action also copies from an earlier removed outer tag into a newly
# pushed inner tag while independently creating the new outer tag.
jq '.service_graph.extended_vlans[0].rules[0].filter_outer =
      {priority: 8, vid: 4096, tpid_dei: 0} |
    .service_graph.extended_vlans[0].rules[0].tags_to_remove = 2 |
    .service_graph.extended_vlans[0].rules[0].treatment_outer =
      {priority: 7, vid: 200, tpid_dei: 4} |
    .service_graph.extended_vlans[0].rules[0].treatment_inner.priority = 9' \
	"$temporary/evto-wildcard-copy.json" > "$temporary/evto-removed-copy.json"
"$AIROHA_OMCI_RENDER" "$temporary/evto-removed-copy.json" \
	> "$temporary/evto-removed-copy.rendered"
awk '$1 == "evto-up" { split($3, f, ":"); ok = f[33] == "outer" }
     END { exit !ok }' "$temporary/evto-removed-copy.rendered"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/evto-removed-copy.json"
grep -q 'vlan transform pop_tags 2 push_tags 2 outer .* inner .*priority outer' \
	"$AIROHA_OMCI_TC_TRANSFORM_LOG"
jq '.service_graph.extended_vlans[0].enhanced_mode = 1 |
    .service_graph.extended_vlans[0].dscp_to_pbit[1] = 1 |
    .service_graph.extended_vlans[0].rules[0].treatment_inner.priority = 10' \
	"$package/tests/desired-v2.json" > "$temporary/evto-dscp-copy.json"
"$AIROHA_OMCI_RENDER" "$temporary/evto-dscp-copy.json" > "$temporary/evto-dscp-copy.rendered"
awk '$1 == "evto-up" {
       split($3, f, ":"); count++;
       if (f[26] == 4 && f[27] == 1 && f[28] == 1 && f[19] == 1) ipv4 = 1;
       if (f[26] == 6 && f[27] == 1 && f[28] == 65 && f[19] == 1) ipv6 = 1
     }
     END { exit !(count == 128 && ipv4 && ipv6) }' "$temporary/evto-dscp-copy.rendered"
awk '$1 == "evto-down" {
       split($3, f, ":"); count++;
       if (f[4] == 0 && f[26] == 0 && f[27] == -1 && f[28] == 0) pbit0 = 1;
       if (f[4] == 1 && f[26] == 0 && f[27] == -1 && f[28] == 1) pbit1 = 1
     }
     END { exit !(count == 2 && pbit0 && pbit1) }' "$temporary/evto-dscp-copy.rendered"

"$AIROHA_OMCI_PLATFORM" apply "$package/tests/desired-v2.json"
[ "$(cat "$temporary/gpon/data_gems")" = '200:2:3 201:2:3' ]

# Class 280 downstream policing runs on PON ingress, selected by the receive
# GEM mark, and must execute before delivery to the resolved UNI.
jq '.service_graph.traffic_descriptors = [{
      entity_id: 34816, cir: 100000, pir: 200000, cbs: 64, pbs: 128,
      colour_mode: 0, ingress_colour_marking: 0,
      egress_colour_marking: 0, meter_type: 0
    }] |
    .service_graph.gem_ports[0].downstream_traffic_descriptor = 34816' \
	"$package/tests/desired-v2.json" > "$temporary/downstream-meter.json"
"$AIROHA_OMCI_RENDER" "$temporary/downstream-meter.json" \
	> "$temporary/downstream-meter.rendered"
grep -qx 'down-meter 200 200000 128' "$temporary/downstream-meter.rendered"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/downstream-meter.json"
tc -s filter show dev pon ingress chain 2010 pref 1200 > "$temporary/downstream-meter.tc"
awk '/action order [0-9]+:[[:space:]]+police/ { police = NR }
     /action order [0-9]+:[[:space:]]+mirred/ { delivery = NR }
     END { exit !(police && delivery && police < delivery) }' \
	"$temporary/downstream-meter.tc"
grep -q 'rate 1600Kbit' "$temporary/downstream-meter.tc"
grep -q 'burst 128b' "$temporary/downstream-meter.tc"
"$AIROHA_OMCI_PLATFORM" apply "$package/tests/desired-v2.json"
if tc filter show dev pon ingress chain 2010 pref 1200 | grep -q 'police'; then
	echo 'removed downstream traffic descriptor left a policer installed' >&2
	exit 1
fi

tc filter show dev lan1 ingress chain 2010 | grep -q 'vlan_prio 0'
tc filter show dev lan1 ingress chain 2010 | grep -q 'mark 2708472008'
tc filter show dev lan1 ingress chain 2010 | grep -q 'mark 2708472009'
tc filter show dev pon ingress chain 2010 | grep -q 'handle 0xa17000c8'
tc filter show dev pon ingress chain 2010 | grep -q 'handle 0xa17000c9'
tc filter show dev lan1 ingress | grep -q 'goto chain 2009'
tc filter show dev lan1 ingress chain 2009 | grep -q 'push id 100'
tc filter show dev lan1 ingress chain 2009 | grep -q 'goto chain 2008'
tc filter show dev lan1 ingress chain 2008 | grep -q 'bpf'
tc filter show dev lan1 ingress chain 2008 | grep -q 'goto chain 2010'
tc filter show dev lan1 egress chain 2012 | grep -q 'bpf'
tc filter show dev lan1 egress chain 2012 | grep -q 'goto chain 2011'
tc filter show dev lan1 egress chain 2011 | grep -q 'pop'
[ "$(cat "$temporary/gpon/data_gems")" = '200:2:3 201:2:3' ]

# DSCP-derived treatment expands to ordered IPv4 and IPv6 classifiers while
# retaining the original EVTO row precedence.
"$AIROHA_OMCI_PLATFORM" apply "$temporary/evto-dscp-copy.json"
tc filter show dev lan1 ingress chain 2009 pref 12 | grep -Eq 'ip_tos (0x)?0?4/0xfc'
tc filter show dev lan1 ingress chain 2009 pref 12 | grep -q 'priority 1'
tc filter show dev lan1 ingress chain 2009 pref 140 | grep -Eq 'ip_tos (0x)?0?4/0xfc'
tc filter show dev lan1 ingress chain 2009 pref 140 | grep -q 'priority 1'
tc filter show dev lan1 egress chain 2011 pref 10 | grep -q 'vlan_prio 0'
tc filter show dev lan1 egress chain 2011 pref 12 | grep -q 'vlan_prio 1'
tc filter show dev lan1 egress chain 2011 pref 12 | grep -q 'pop'

# Downstream modes 3/6 inverse only the VID and preserve any P-bit on a
# translated single-tag service.
jq '.service_graph.extended_vlans[0].downstream_mode = 3 |
    .service_graph.extended_vlans[0].rules[0].filter_inner =
      {priority: 8, vid: 10, tpid_dei: 0} |
    .service_graph.extended_vlans[0].rules[0].tags_to_remove = 1 |
    .service_graph.extended_vlans[0].rules[0].treatment_inner =
      {priority: 5, vid: 100, tpid_dei: 4}' \
	"$package/tests/desired-v2.json" > "$temporary/evto-vid-only.json"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/evto-vid-only.json"
tc filter show dev lan1 ingress chain 2009 | grep -q 'vlan_id 10'
tc filter show dev lan1 ingress chain 2009 | grep -q 'modify id 100'
tc filter show dev lan1 ingress chain 2009 | grep -q 'priority 5'
tc filter show dev lan1 egress chain 2011 | grep -q 'vlan_id 100'
grep -q 'vlan transform pop_tags 1 push_tags 1 outer .*id 10 priority outer.*dei outer' \
	"$AIROHA_OMCI_TC_TRANSFORM_LOG"
ip link add link olt name olt.100 type vlan id 100
ip link set olt.100 master vrf-olt
ip link set olt.100 up
tc qdisc add dev pon clsact 2>/dev/null || true
tc filter replace dev pon ingress protocol all pref 1 handle 1 flower skip_hw \
	action skbedit mark 0xa17000c8 action continue

# Filter TPID/DEI values 6 and 7 select input TPID frames with DEI clear or
# set in the same flower classifier as VID, PCP and protocol criteria.
jq '.service_graph.extended_vlans[0].rules[0].filter_inner =
      {priority: 0, vid: 100, tpid_dei: 7} |
    .service_graph.extended_vlans[0].downstream_mode = 1 |
    .service_graph.extended_vlans[0].rules[0].tags_to_remove = 1 |
    .service_graph.extended_vlans[0].rules[0].treatment_inner =
      {priority: 0, vid: 100, tpid_dei: 4}' \
	"$package/tests/desired-v2.json" > "$temporary/evto-dei.json"
export AIROHA_OMCI_TC_DEI_LOG="$temporary/tc-dei-filter.log"
export AIROHA_OMCI_TC="$package/tests/tc-host-dei"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/evto-dei.json"
grep -qx 'vlan_dei 1' "$AIROHA_OMCI_TC_DEI_LOG"
tc filter show dev lan1 ingress chain 2009 | grep -q 'vlan_id 100'
if tc filter show dev lan1 ingress chain 2009 | grep -q 'bpf'; then
	echo 'EVTO DEI filter unexpectedly uses a non-composable BPF child chain' >&2
	exit 1
fi
[ ! -s "$temporary/state/platform-evto-chains" ]

# Emulate the GEM mark normally supplied by the Airoha RX descriptor.
tc qdisc add dev pon clsact 2>/dev/null || true
tc filter replace dev pon ingress protocol all pref 1 handle 1 flower skip_hw \
	action skbedit mark 0xa17000c8 action continue
ip address add 192.0.2.1/24 dev cpe
ip address add 192.0.2.2/24 dev olt.100

# Resolve an 802.1p mapper through a MAC bridge profile. Both UNI ports share
# the physical ANI, while OMCI port bridging keeps the UNI ports isolated.
jq '.service_graph.unis += [{
      entity_id: 258, interface: "lan2", administrative_state: 0,
      operational_state: 0, configuration: 4
    }] |
    .service_graph.pbit_mappers[0].tp_type = 0 |
    .service_graph.pbit_mappers[0].tp_pointer = 65535 |
    .service_graph.bridges[0].ports += [{
      entity_id: 1281, port: 2, tp_type: 1, tp: 258, priority: 128,
      path_cost: 10, spanning_tree: 0, outbound_td: 65535,
      inbound_td: 65535, mac_learning_depth: 0
    }, {
      entity_id: 1282, port: 3, tp_type: 3, tp: 512, priority: 128,
      path_cost: 10, spanning_tree: 0, outbound_td: 65535,
      inbound_td: 65535, mac_learning_depth: 0
    }]' "$package/tests/desired-v2.json" > "$temporary/bridge.json"

"$AIROHA_OMCI_PLATFORM" clear
"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge.json"
jq -e '.state == "applied" and .bridge_count == 1 and
       .configured_bridges == "omb0100" and .classifiers > 0' \
	"$temporary/state/platform.json" >/dev/null
ip link show dev lan1 | grep -q 'master omb0100'
ip link show dev lan2 | grep -q 'master omb0100'
ip link show dev oma0100 | grep -q 'master omb0100'
ip link show dev pon | grep -vq ' master '
bridge -details link show dev lan1 | grep -q 'learning on flood on.*isolated on'
bridge -details link show dev lan2 | grep -q 'learning on flood on.*isolated on'
bridge -details link show dev oma0100 | grep -q 'learning on flood on.*isolated off'
tc filter show dev omp0100 ingress | grep -qi 'mirred.*egress redirect.*pon'
tc filter show dev lan1 ingress chain 2010 | grep -q 'gact action pass'
if tc filter show dev lan1 ingress chain 2010 | grep -q 'mirred'; then
	echo 'bridged upstream classifier still redirects to PON' >&2
	exit 1
fi
tc filter show dev pon ingress chain 2010 pref 1200 | grep -qi 'mirred.*egress redirect.*omp0100'
tc filter show dev pon ingress chain 2010 | grep -q 'gact action drop'
ping -I cpe -c 1 -W 1 192.0.2.2 >/dev/null
cpe_mac="$(ip -o link show dev cpe)"; cpe_mac="${cpe_mac#* link/ether }"; cpe_mac="${cpe_mac%% *}"
cpe2_mac="$(ip -o link show dev cpe2)"; cpe2_mac="${cpe2_mac#* link/ether }"; cpe2_mac="${cpe2_mac%% *}"
olt_mac="$(ip -o link show dev olt)"; olt_mac="${olt_mac#* link/ether }"; olt_mac="${olt_mac%% *}"
bridge fdb show br omb0100 | grep -qi "$cpe_mac dev lan1"
bridge fdb show br omb0100 | grep -qi "$olt_mac dev oma0100"

# Class-47 aggregate descriptors police traffic entering and leaving the
# bridge on both UNI and ANI logical ports.
jq '.service_graph.traffic_descriptors = [
      {entity_id: 34816, cir: 100000, pir: 125000, cbs: 2048, pbs: 4096,
       colour_mode: 0, ingress_colour_marking: 0, egress_colour_marking: 0, meter_type: 2},
      {entity_id: 34817, cir: 200000, pir: 250000, cbs: 4096, pbs: 8192,
       colour_mode: 0, ingress_colour_marking: 0, egress_colour_marking: 0, meter_type: 2}
    ] |
    .service_graph.bridges[0].ports[0].outbound_td = 34816 |
    .service_graph.bridges[0].ports[0].inbound_td = 34817 |
    .service_graph.bridges[0].ports[2].outbound_td = 34817 |
    .service_graph.bridges[0].ports[2].inbound_td = 34816' \
	"$temporary/bridge.json" > "$temporary/bridge-port-meter.json"
"$AIROHA_OMCI_RENDER" "$temporary/bridge-port-meter.json" \
	> "$temporary/bridge-port-meter.rendered"
grep -qx 'port-meter lan1 egress 125000:4096' "$temporary/bridge-port-meter.rendered"
grep -qx 'port-meter lan1 ingress 250000:8192' "$temporary/bridge-port-meter.rendered"
grep -qx 'port-meter oma0100 egress 250000:8192' "$temporary/bridge-port-meter.rendered"
grep -qx 'port-meter oma0100 ingress 125000:4096' "$temporary/bridge-port-meter.rendered"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge-port-meter.json"
tc filter show dev lan1 egress pref 1800 | grep -q 'police'
tc filter show dev lan1 ingress pref 1800 | grep -q 'police'
tc filter show dev oma0100 egress pref 1800 | grep -q 'police'
tc filter show dev oma0100 ingress pref 1800 | grep -q 'police'
"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge.json"
if tc filter show dev lan1 ingress pref 1800 2>/dev/null | grep -q 'police' ||
   tc filter show dev oma0100 egress pref 1800 2>/dev/null | grep -q 'police'; then
	echo 'MAC bridge port traffic descriptor survived an unmetered graph' >&2
	exit 1
fi

# Class 298 meters bridge-forwarded upstream traffic on the ANI endpoint. The
# three matches are mutually exclusive: broadcast has l2_miss 0, while unknown
# unicast and multicast use complementary destination-MAC masks with l2_miss 1.
jq '.service_graph.traffic_descriptors = [
      {entity_id: 34816, cir: 100000, pir: 125000, cbs: 2048, pbs: 4096,
       colour_mode: 0, ingress_colour_marking: 0, egress_colour_marking: 0, meter_type: 2},
      {entity_id: 34817, cir: 200000, pir: 250000, cbs: 4096, pbs: 8192,
       colour_mode: 0, ingress_colour_marking: 0, egress_colour_marking: 0, meter_type: 2},
      {entity_id: 34818, cir: 300000, pir: 375000, cbs: 8192, pbs: 16384,
       colour_mode: 0, ingress_colour_marking: 0, egress_colour_marking: 0, meter_type: 2}
    ] |
    .service_graph.dot1_rate_limiters = [{
      entity_id: 35072, parent_me: 256, tp_type: 1,
      upstream_unicast_flood_traffic_descriptor: 34816,
      upstream_broadcast_traffic_descriptor: 34817,
      upstream_multicast_payload_traffic_descriptor: 34818
    }]' "$temporary/bridge.json" > "$temporary/rate-limit.json"
"$AIROHA_OMCI_RENDER" "$temporary/rate-limit.json" > "$temporary/rate-limit.rendered"
grep -qx 'rate-limit oma0100 unknown 125000:4096' "$temporary/rate-limit.rendered"
grep -qx 'rate-limit oma0100 broadcast 250000:8192' "$temporary/rate-limit.rendered"
grep -qx 'rate-limit oma0100 multicast 375000:16384' "$temporary/rate-limit.rendered"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/rate-limit.json"
tc filter show dev oma0100 egress pref 1700 | grep -q 'l2_miss 1'
tc filter show dev oma0100 egress pref 1700 | grep -q 'dst_mac 00:00:00:00:00:00/01:00:00:00:00:00'
tc filter show dev oma0100 egress pref 1700 | grep -q 'police'
tc filter show dev oma0100 egress pref 1701 | grep -q 'l2_miss 0'
tc filter show dev oma0100 egress pref 1701 | grep -q 'dst_mac ff:ff:ff:ff:ff:ff'
tc filter show dev oma0100 egress pref 1701 | grep -q 'police'
tc filter show dev oma0100 egress pref 1702 | grep -q 'l2_miss 1'
tc filter show dev oma0100 egress pref 1702 | grep -q 'dst_mac 01:00:00:00:00:00/01:00:00:00:00:00'
tc filter show dev oma0100 egress pref 1702 | grep -q 'police'

jq '.service_graph.traffic_descriptors[0].pir = 0' \
	"$temporary/rate-limit.json" > "$temporary/rate-limit-zero-pir.json"
if "$AIROHA_OMCI_RENDER" "$temporary/rate-limit-zero-pir.json" >/dev/null 2>&1; then
	echo 'dot1 rate limiter with factory-policy PIR was accepted' >&2
	exit 1
fi
jq '.service_graph.traffic_descriptors = [{
      entity_id: 34816, cir: 100000, pir: 125000, cbs: 2048, pbs: 4096,
      colour_mode: 0, ingress_colour_marking: 0, egress_colour_marking: 0, meter_type: 2
    }] |
    .service_graph.dot1_rate_limiters = [{
      entity_id: 35072, parent_me: 512, tp_type: 2,
      upstream_unicast_flood_traffic_descriptor: 34816,
      upstream_broadcast_traffic_descriptor: 65535,
      upstream_multicast_payload_traffic_descriptor: 65535
    }]' \
	"$package/tests/desired-v2.json" > "$temporary/rate-limit-direct-mapper.json"
"$AIROHA_OMCI_RENDER" "$temporary/rate-limit-direct-mapper.json" \
	> "$temporary/rate-limit-direct-mapper.rendered"
grep -qx 'bridge omm0200 512 0:1:0:32768:2000:200:1500:0:30000:0' \
	"$temporary/rate-limit-direct-mapper.rendered"
grep -qx 'bridge-ani omm0200 omx0200 omy0200' \
	"$temporary/rate-limit-direct-mapper.rendered"
grep -qx 'rate-limit omx0200 unknown 125000:4096' \
	"$temporary/rate-limit-direct-mapper.rendered"
grep -qx 'down 200 omy0200 0' "$temporary/rate-limit-direct-mapper.rendered"
grep -qx 'down 201 omy0200 0' "$temporary/rate-limit-direct-mapper.rendered"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/rate-limit-direct-mapper.json"
jq -e '.state == "applied" and .bridge_count == 1 and
       .configured_bridges == "omm0200"' "$temporary/state/platform.json" >/dev/null
ip link show dev lan1 | grep -q 'master omm0200'
ip link show dev omx0200 | grep -q 'master omm0200'
tc filter show dev omy0200 ingress | grep -qi 'mirred.*egress redirect.*pon'
tc filter show dev omx0200 egress pref 1700 | grep -q 'l2_miss 1'
tc filter show dev omx0200 egress pref 1700 | grep -q 'police'
tc filter show dev pon ingress chain 2010 pref 1200 | grep -qi 'mirred.*egress redirect.*omy0200'
ping -I cpe -c 1 -W 1 192.0.2.2 >/dev/null
bridge fdb show br omm0200 | grep -qi "$cpe_mac dev lan1"
bridge fdb show br omm0200 | grep -qi "$olt_mac dev omx0200"

# Restore the unmetered bridge before the multicast and bridge-policy cases.
"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge.json"
if ip link show dev omm0200 >/dev/null 2>&1; then
	echo 'direct-mapper FDB endpoint survived bridge replacement' >&2
	exit 1
fi
if tc filter show dev oma0100 egress 2>/dev/null | grep -q 'police'; then
	echo 'dot1 rate limiter survived an unmetered replacement graph' >&2
	exit 1
fi

# A multicast GEM IW uses MAC bridge TP type 6. Its address table can add
# implicit downstream-only GEM Port-IDs without creating extra GEM CTP MEs.
# Native OMCI multicast guards consume reports and default-deny downstream
# copies until airoha-mcastd installs static or authorized dynamic rules.
jq '.service_graph.multicast_gem_interworking = [{
      entity_id: 770, gem_port: 1025, port_id: 201, tcont: 0,
      alloc_id: 100, option: 1, service_profile: 256, gal_profile: 1,
      ipv4_ranges: [
        {gem_port_id: 201, secondary_key: 0, start: "224.0.0.0", stop: "239.255.255.255"},
        {gem_port_id: 202, secondary_key: 1, start: "225.0.0.0", stop: "225.0.0.255"}
      ],
      ipv6_ranges: []
    }] |
    .service_graph.multicast_operations_profiles = [{
      entity_id: 1792, igmp_version: 3, igmp_function: 0,
      immediate_leave: 1, upstream_tci: 0, upstream_tag_control: 0,
      upstream_rate: 0, robustness: 2, querier_ip_address: 0,
      query_interval: 125, query_max_response_time: 10,
      last_member_query_interval: 1, unauthorized_join_behaviour: 0,
      downstream_tag_control: 0, downstream_tci: 0
    }] |
    .service_graph.multicast_subscribers = [{
      entity_id: 1280, me_type: 0, profile: 1792,
      max_simultaneous_groups: 0, max_multicast_bandwidth: 0,
      bandwidth_enforcement: 0
    }] |
    .service_graph.bridges[0].ports += [{
      entity_id: 1283, port: 4, tp_type: 6, tp: 770, priority: 128,
      path_cost: 10, spanning_tree: 0, outbound_td: 65535,
      inbound_td: 65535, mac_learning_depth: 0
    }]' "$temporary/bridge.json" > "$temporary/multicast-bridge.json"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/multicast-bridge.json"
[ "$(cat "$temporary/gpon/data_gems")" = '200:2:3 201:2:3 202:2:1' ]
tc filter show dev pon ingress chain 2010 pref 1202 | grep -q 'handle 0xa17000ca'
tc filter show dev pon ingress chain 2010 pref 1202 | grep -qi 'mirred.*egress redirect.*omp0100'
ip -details link show dev omb0100 | grep -q 'mcast_snooping 0'
tc filter show dev lan1 ingress pref 1900 | grep -Eq 'ip_proto ((0x)?02|igmp)'
tc filter show dev lan1 egress chain 2013 pref 65000 | grep -q 'gact action drop'

# Logical ANI ports sharing one Linux bridge endpoint must have one learning
# depth, otherwise the graph is rejected before any platform state changes.
jq '(.service_graph.bridges[0].ports[] | select(.tp_type == 6) |
      .mac_learning_depth) = 1' \
	"$temporary/multicast-bridge.json" > "$temporary/ani-depth-conflict.json"
if "$AIROHA_OMCI_RENDER" "$temporary/ani-depth-conflict.json" >/dev/null 2>&1; then
	echo 'inconsistent shared ANI learning depths were accepted' >&2
	exit 1
fi

jq '.service_graph.traffic_descriptors = [
      {entity_id: 34816, cir: 0, pir: 125000, cbs: 0, pbs: 4096},
      {entity_id: 34817, cir: 0, pir: 250000, cbs: 0, pbs: 8192}
    ] |
    (.service_graph.bridges[0].ports[] | select(.tp_type == 3) |
      .outbound_td) = 34816 |
    (.service_graph.bridges[0].ports[] | select(.tp_type == 6) |
      .outbound_td) = 34817' \
	"$temporary/multicast-bridge.json" > "$temporary/ani-meter-conflict.json"
if "$AIROHA_OMCI_RENDER" "$temporary/ani-meter-conflict.json" >/dev/null 2>&1; then
	echo 'inconsistent shared ANI traffic descriptors were accepted' >&2
	exit 1
fi

"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge.json"
[ "$(cat "$temporary/gpon/data_gems")" = '200:2:3 201:2:3' ]

# The target bridge exposes the G.988 profile and per-port learning caps. The
# host kernel predates both attributes, so wrappers record the target commands
# while delegating every other operation to the host iproute2 binaries.
jq '.service_graph.bridges[0].mac_learning_depth = 64 |
    .service_graph.bridges[0].ports[].mac_learning_depth = 32' \
	"$temporary/bridge.json" > "$temporary/bridge-learning-depth.json"
export AIROHA_OMCI_IP="$package/tests/ip-fdb-max"
export AIROHA_OMCI_IP_LOG="$temporary/ip-fdb-max.log"
export AIROHA_OMCI_BRIDGE="$package/tests/bridge-fdb-max"
export AIROHA_OMCI_BRIDGE_LOG="$temporary/bridge-fdb-max.log"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge-learning-depth.json"
grep -qx 'link set dev omb0100 type bridge fdb_max_learned 64' "$AIROHA_OMCI_IP_LOG"
grep -qx 'link set dev lan1 fdb_max_learned 32' "$AIROHA_OMCI_BRIDGE_LOG"
grep -qx 'link set dev lan2 fdb_max_learned 32' "$AIROHA_OMCI_BRIDGE_LOG"
grep -qx 'link set dev oma0100 fdb_max_learned 32' "$AIROHA_OMCI_BRIDGE_LOG"
unset AIROHA_OMCI_IP AIROHA_OMCI_IP_LOG AIROHA_OMCI_BRIDGE AIROHA_OMCI_BRIDGE_LOG

# G.988 associates mapper and GEM-IW EVTO profiles with the ANI side of a
# configured MAC bridge. Their upstream rules run on ANI egress and their
# downstream inverse rules run on ANI ingress.
jq '.service_graph.vlan_filters = [] |
    .service_graph.extended_vlans[0].association_type = 1 |
    .service_graph.extended_vlans[0].associated_class = 130 |
    .service_graph.extended_vlans[0].associated_me = 512' \
	"$temporary/bridge.json" > "$temporary/bridge-mapper-evto.json"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge-mapper-evto.json"
tc filter show dev oma0100 egress chain 2009 | grep -q 'push id 100'
tc filter show dev oma0100 ingress chain 2011 | grep -q 'pop'
if tc filter show dev oma0100 ingress chain 2011 | grep -q 'bpf'; then
	echo 'mapper EVTO inverse installed an unnecessary DEI gate' >&2
	exit 1
fi
ping -I cpe -c 1 -W 1 192.0.2.2 >/dev/null

jq '.service_graph.extended_vlans[0].association_type = 5 |
    .service_graph.extended_vlans[0].associated_class = 266 |
    .service_graph.extended_vlans[0].associated_me = 768' \
	"$temporary/bridge-mapper-evto.json" > "$temporary/bridge-gem-evto.json"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge-gem-evto.json"
tc filter show dev oma0100 egress chain 2009 | grep -q 'handle 0xa17000c8'
tc filter show dev oma0100 egress chain 2009 | grep -q 'goto chain 10200'
tc filter show dev oma0100 egress chain 10200 | grep -q 'push id 100'
tc filter show dev oma0100 ingress chain 2011 | grep -q 'handle 0xa17000c8'
tc filter show dev oma0100 ingress chain 2011 | grep -q 'goto chain 15200'
tc filter show dev oma0100 ingress chain 15200 | grep -q 'pop'
if tc filter show dev oma0100 ingress chain 15200 | grep -q 'bpf'; then
	echo 'GEM EVTO inverse installed an unnecessary DEI gate' >&2
	exit 1
fi
ping -I cpe -c 1 -W 1 192.0.2.2 >/dev/null

# Distinct GEM-IW associations on one ANI keep independent match and action
# chains, so adding a second service cannot replace the first one's rules.
jq '.service_graph.extended_vlans += [(.service_graph.extended_vlans[0] |
      .entity_id = 1537 | .associated_me = 769)]' \
	"$temporary/bridge-gem-evto.json" > "$temporary/bridge-multi-gem-evto.json"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge-multi-gem-evto.json"
tc filter show dev oma0100 egress chain 2009 | grep -q 'handle 0xa17000c8'
tc filter show dev oma0100 egress chain 2009 | grep -q 'handle 0xa17000c9'
tc filter show dev oma0100 egress chain 10200 | grep -q 'push id 100'
tc filter show dev oma0100 egress chain 10201 | grep -q 'push id 100'
tc filter show dev oma0100 ingress chain 2011 | grep -q 'handle 0xa17000c8'
tc filter show dev oma0100 ingress chain 2011 | grep -q 'handle 0xa17000c9'
tc filter show dev oma0100 ingress chain 15200 | grep -q 'pop'
tc filter show dev oma0100 ingress chain 15201 | grep -q 'pop'
if tc filter show dev oma0100 ingress chain 15200 | grep -q 'bpf' ||
   tc filter show dev oma0100 ingress chain 15201 | grep -q 'bpf'; then
	echo 'multi-GEM EVTO inverse installed an unnecessary DEI gate' >&2
	exit 1
fi

"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge.json"

ip link add link cpe2 name cpe2.100 type vlan id 100
ip link set cpe2.100 master vrf-cpe2
ip link set cpe2.100 up
ip address add 203.0.113.1/24 dev cpe2.100
ip address add 203.0.113.2/24 dev olt.100
ping -I cpe2.100 -c 1 -W 1 203.0.113.2 >/dev/null
ip address add 10.0.0.1/24 dev cpe
ip address add 10.0.0.2/24 dev cpe2.100
if ping -I cpe -c 1 -W 1 10.0.0.2 >/dev/null; then
	echo 'OMCI port_bridging=0 admitted LAN-to-LAN traffic' >&2
	exit 1
fi

jq '.service_graph.bridges[0].port_bridging = 1' \
	"$temporary/bridge.json" > "$temporary/bridge-port-forward.json"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge-port-forward.json"
bridge -details link show dev lan1 | grep -q 'isolated off'
ip neighbour flush dev cpe to 10.0.0.2 >/dev/null
ping -I cpe -c 1 -W 1 10.0.0.2 >/dev/null

jq '.service_graph.bridges[0].unknown_mac_discard = 1' \
	"$temporary/bridge-port-forward.json" > "$temporary/bridge-unknown-discard.json"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge-unknown-discard.json"
bridge -details link show dev lan1 | grep -q 'flood off'
bridge -details link show dev lan2 | grep -q 'flood off'
bridge -details link show dev oma0100 | grep -q 'flood off'

jq '.service_graph.vlan_filters[0] = {
      entity_id: 1280, bridge_port: 1280, forward_operation: 22,
      tagged_action: "j", tagged_criterion: "vid", untagged_action: "a",
      entries: [100]
    }' "$temporary/bridge-port-forward.json" > "$temporary/bridge-action-j.json"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge-action-j.json"
bridge -details link show dev lan1 | grep -q 'flood off'
bridge -details link show dev lan2 | grep -q 'flood on'
"$AIROHA_OMCI_PLATFORM" clear
if ip link show dev omb0100 >/dev/null 2>&1; then
	echo 'managed bridge survived clear' >&2
	exit 1
fi
for interface in lan1 lan2 pon; do
	if ip -o link show dev "$interface" | grep -q ' master '; then
		echo "$interface remained enslaved after clear" >&2
		exit 1
	fi
done
for interface in oma0100 omp0100; do
	if ip link show dev "$interface" >/dev/null 2>&1; then
		echo "$interface survived bridge clear" >&2
		exit 1
	fi
done

# Two independent OLT bridge profiles must share the physical PON without
# merging their learning domains or assigning pon to two Linux bridges.
jq '.service_graph |= (
      .unis += [{
        entity_id: 258, interface: "lan2", administrative_state: 0,
        operational_state: 0, configuration: 4
      }] |
      .pbit_mappers = [] |
      .gem_interworking = [
        { entity_id: 768, gem_port: 1024, option: 1, service_profile: 256 },
        { entity_id: 769, gem_port: 1025, option: 1, service_profile: 257 }
      ] |
      .bridges[0] as $bridge |
      .bridges = [
        ($bridge | .ports = [{
          entity_id: 1280, port: 1, tp_type: 1, tp: 257, priority: 128,
          path_cost: 10, spanning_tree: 0, outbound_td: 65535,
          inbound_td: 65535, mac_learning_depth: 0
        }, {
          entity_id: 1282, port: 2, tp_type: 5, tp: 768, priority: 128,
          path_cost: 10, spanning_tree: 0, outbound_td: 65535,
          inbound_td: 65535, mac_learning_depth: 0
        }]),
        ($bridge | .entity_id = 257 | .ports = [{
          entity_id: 1281, port: 1, tp_type: 1, tp: 258, priority: 128,
          path_cost: 10, spanning_tree: 0, outbound_td: 65535,
          inbound_td: 65535, mac_learning_depth: 0
        }, {
          entity_id: 1283, port: 2, tp_type: 5, tp: 769, priority: 128,
          path_cost: 10, spanning_tree: 0, outbound_td: 65535,
          inbound_td: 65535, mac_learning_depth: 0
        }])
      ] |
      .vlan_filters = [] |
      .extended_vlans = []
    )' "$package/tests/desired-v2.json" > "$temporary/multi-bridge.json"

"$AIROHA_OMCI_PLATFORM" apply "$temporary/multi-bridge.json"
jq -e '.state == "applied" and .bridge_count == 2 and
       .configured_bridges == "omb0100 omb0101"' \
	"$temporary/state/platform.json" >/dev/null
ip link show dev lan1 | grep -q 'master omb0100'
ip link show dev lan2 | grep -q 'master omb0101'
ip link show dev oma0100 | grep -q 'master omb0100'
ip link show dev oma0101 | grep -q 'master omb0101'
ip link show dev pon | grep -vq ' master '
tc filter show dev omp0100 ingress | grep -qi 'mirred.*egress redirect.*pon'
tc filter show dev omp0101 ingress | grep -qi 'mirred.*egress redirect.*pon'
tc filter show dev lan1 ingress chain 2010 | grep -q 'mark 2708472008'
tc filter show dev lan2 ingress chain 2010 | grep -q 'mark 2708472009'
tc filter show dev pon ingress chain 2010 pref 1200 | grep -q 'handle 0xa17000c8'
tc filter show dev pon ingress chain 2010 pref 1200 | grep -qi 'mirred.*egress redirect.*omp0100'
tc filter show dev pon ingress chain 2010 pref 1201 | grep -q 'handle 0xa17000c9'
tc filter show dev pon ingress chain 2010 pref 1201 | grep -qi 'mirred.*egress redirect.*omp0101'

ip address add 10.10.1.1/24 dev cpe
ip address add 10.10.1.2/24 dev olt
if ! ping -I cpe -c 1 -W 1 10.10.1.2 >/dev/null; then
	ping -I cpe -c 1 -W 1 10.10.1.2 || true
	tc -s filter show dev lan1 ingress chain 2010
	tc -s filter show dev omp0100 ingress
	tc -s filter show dev pon ingress chain 2010
	bridge fdb show br omb0100
	ip address show dev cpe
	ip address show dev olt
	ip neighbour show
	exit 1
fi
tc filter replace dev pon ingress protocol all pref 1 handle 1 flower skip_hw \
	action skbedit mark 0xa17000c9 action continue
ip address add 10.10.2.1/24 dev cpe2
ip address add 10.10.2.2/24 dev olt
if ! ping -I cpe2 -c 1 -W 1 10.10.2.2 >/dev/null; then
	ping -I cpe2 -c 1 -W 1 10.10.2.2 || true
	tc -s filter show dev lan2 ingress chain 2010
	tc -s filter show dev omp0101 ingress
	tc -s filter show dev pon ingress chain 2010
	bridge fdb show br omb0101
	ip address show dev cpe2
	ip address show dev olt
	ip neighbour show
	exit 1
fi
bridge fdb show br omb0100 | grep -qi "$cpe_mac dev lan1"
bridge fdb show br omb0101 | grep -qi "$cpe2_mac dev lan2"
if bridge fdb show br omb0100 | grep -qi "$cpe2_mac" ||
   bridge fdb show br omb0101 | grep -qi "$cpe_mac"; then
	echo 'independent OMCI bridge profiles shared an FDB entry' >&2
	exit 1
fi

"$AIROHA_OMCI_PLATFORM" clear
for interface in oma0100 omp0100 oma0101 omp0101; do
	if ip link show dev "$interface" >/dev/null 2>&1; then
		echo "$interface survived multi-bridge clear" >&2
		exit 1
	fi
done

# Never take a port away from a user or netifd-owned bridge implicitly.
ip link add name br-user type bridge
ip link set dev br-user up
ip link set dev lan1 master br-user
if "$AIROHA_OMCI_PLATFORM" apply "$temporary/bridge.json"; then
	echo 'OMCI bridge took ownership from a non-OMCI bridge' >&2
	exit 1
fi
jq -e '.state == "bridge-conflict" and .bridge_count == 0' \
	"$temporary/state/platform.json" >/dev/null
ip link show dev lan1 | grep -q 'master br-user'
[ "$(cat "$temporary/gpon/data_gems")" = off ]
ip link delete dev br-user type bridge

# Emulate the GEM mark normally supplied by the Airoha RX descriptor and
# verify that the non-zero OMCI chain actually forwards both directions.
"$AIROHA_OMCI_PLATFORM" apply "$package/tests/desired-v2.json"
if ! ping -I cpe -c 1 -W 1 192.0.2.2 >/dev/null; then
	tc -s filter show dev lan1 ingress chain 2009
	tc -s filter show dev pon ingress chain 2010
	tc -s filter show dev lan1 egress chain 2011
	ip address show dev cpe
	ip address show dev olt.100
	ip route show table 10
	ip route show table 20
	ip neighbour show
	exit 1
fi

# A downstream frame outside the positive VLAN filter must be discarded
# before the inverse EVTO rule can deliver it to the UNI.
ip link add link olt name olt.200 type vlan id 200
ip link set olt.200 master vrf-olt
ip link set olt.200 up
ip address add 198.51.100.1/24 dev cpe
ip address add 198.51.100.2/24 dev olt.200
if ping -I olt.200 -c 1 -W 1 198.51.100.1 >/dev/null; then
	echo 'VLAN filter admitted disallowed VID 200' >&2
	exit 1
fi
tc -s filter show dev lan1 egress chain 2012 | grep -Eq 'Sent [1-9][0-9]* bytes'

"$AIROHA_OMCI_PLATFORM" clear
[ "$(cat "$temporary/gpon/data_gems")" = off ]
[ -z "$(tc filter show dev pon ingress chain 2010)" ]
[ -z "$(tc filter show dev lan1 ingress chain 2010)" ]
[ -z "$(tc filter show dev lan1 ingress chain 2009)" ]
[ -z "$(tc filter show dev lan1 ingress chain 2008)" ]
[ -z "$(tc filter show dev lan1 egress chain 2011)" ]
[ -z "$(tc filter show dev lan1 egress chain 2012)" ]

# Exercise legacy class 78 without an extended VLAN operation on the same UNI.
jq '.service_graph.extended_vlans = [] |
    .service_graph.vlan_operations = [{
      entity_id: 257,
      association_type: 0,
      associated_class: 11,
      associated_me: 257,
      upstream_mode: 1,
      upstream_tci: 100,
      downstream_mode: 1
    }]' "$package/tests/desired-v2.json" > "$temporary/classic-vlan.json"
export AIROHA_OMCI_TC_DEI_LOG="$temporary/tc-dei.log"
export AIROHA_OMCI_TC="$package/tests/tc-host-dei"
"$AIROHA_OMCI_PLATFORM" apply "$temporary/classic-vlan.json"
grep -qx 'dei 0' "$AIROHA_OMCI_TC_DEI_LOG"
tc filter show dev lan1 ingress chain 2009 | grep -q 'push id 100'
tc filter show dev lan1 ingress chain 2009 | grep -q 'modify id 100'
tc filter show dev lan1 egress chain 2011 | grep -q 'pop'
ping -I cpe -c 1 -W 1 192.0.2.2 >/dev/null
"$AIROHA_OMCI_PLATFORM" clear

"$AIROHA_OMCI_PLATFORM" apply "$package/tests/desired-v2.json"
"$AIROHA_OMCI_PLATFORM" clear
sh "$package/files/airoha-omci-apply" < "$package/tests/desired-v2.json"
cmp -s "$package/tests/desired-v2.json" "$temporary/state/desired.json"

export AIROHA_OMCI_TC_FAILED="$temporary/tc-downstream-meter-failed"
export AIROHA_OMCI_TC="$package/tests/tc-fail-police-once"
if sh "$package/files/airoha-omci-apply" < "$temporary/downstream-meter.json"; then
	echo 'injected downstream policer failure was accepted' >&2
	exit 1
fi
cmp -s "$package/tests/desired-v2.json" "$temporary/state/desired.json"
[ -e "$AIROHA_OMCI_TC_FAILED" ]
if tc filter show dev pon ingress chain 2010 pref 1200 | grep -q 'police'; then
	echo 'failed downstream policer candidate survived rollback' >&2
	exit 1
fi
export AIROHA_OMCI_TC="$package/tests/tc-host-dei"

export AIROHA_OMCI_TC_FAILED="$temporary/tc-direct-mapper-rate-failed"
export AIROHA_OMCI_TC="$package/tests/tc-fail-police-once"
if sh "$package/files/airoha-omci-apply" < "$temporary/rate-limit-direct-mapper.json"; then
	echo 'injected direct-mapper policer failure was accepted' >&2
	exit 1
fi
cmp -s "$package/tests/desired-v2.json" "$temporary/state/desired.json"
[ -e "$AIROHA_OMCI_TC_FAILED" ]
if ip link show dev omm0200 >/dev/null 2>&1; then
	echo 'failed direct-mapper FDB candidate survived rollback' >&2
	exit 1
fi
export AIROHA_OMCI_TC="$package/tests/tc-host-dei"

export AIROHA_OMCI_TC_FAILED="$temporary/tc-port-meter-failed"
export AIROHA_OMCI_TC="$package/tests/tc-fail-police-once"
if sh "$package/files/airoha-omci-apply" < "$temporary/bridge-port-meter.json"; then
	echo 'injected bridge-port policer failure was accepted' >&2
	exit 1
fi
cmp -s "$package/tests/desired-v2.json" "$temporary/state/desired.json"
[ -e "$AIROHA_OMCI_TC_FAILED" ]
if tc filter show dev lan1 ingress pref 1800 2>/dev/null | grep -q 'police'; then
	echo 'failed bridge-port policer candidate survived rollback' >&2
	exit 1
fi
export AIROHA_OMCI_TC="$package/tests/tc-host-dei"

if sh "$package/files/airoha-omci-apply" < "$package/tests/invalid-v1.json"; then
	echo 'invalid ABI was accepted' >&2
	exit 1
fi
cmp -s "$package/tests/desired-v2.json" "$temporary/state/desired.json"
[ "$(cat "$temporary/gpon/data_gems")" = '200:2:3 201:2:3' ]

# ABI 6 has no PON state domain and must not be interpreted as ABI 7.
jq '.version = 6 | del(.state_domain) | del(.mib_state.state_domain)' \
	"$package/tests/desired-v2.json" > "$temporary/invalid-v6.json"
if sh "$package/files/airoha-omci-apply" < "$temporary/invalid-v6.json"; then
	echo 'platform ABI 6 was accepted as ABI 7' >&2
	exit 1
fi
cmp -s "$package/tests/desired-v2.json" "$temporary/state/desired.json"
[ "$(cat "$temporary/gpon/data_gems")" = '200:2:3 201:2:3' ]

# Software lifecycle commands update the MIB/data-sync transactionally but do
# not rebuild an unchanged GEM, bridge or VLAN graph.
jq '.operation = "command" | .mib_data_sync = 8 |
    .mib_state.mib_data_sync = 8 |
    .mib_state.instances[0].attributes[0].unsigned = 8' \
	"$package/tests/desired-v2.json" > "$temporary/command.json"
"$AIROHA_OMCI_RENDER" "$package/tests/desired-v2.json" > "$temporary/service.rendered"
"$AIROHA_OMCI_RENDER" "$temporary/command.json" > "$temporary/command.rendered"
cmp -s "$temporary/service.rendered" "$temporary/command.rendered"
export AIROHA_OMCI_PLATFORM=/bin/false
sh "$package/files/airoha-omci-apply" record < "$temporary/command.json"
cmp -s "$temporary/command.json" "$temporary/state/desired.json"
cmp -s "$temporary/command.json" "$AIROHA_OMCI_ONU3_STATE"

# A failed non-volatile rename must fail the command and restore both copies.
jq '.mib_data_sync = 9 | .mib_state.mib_data_sync = 9 |
    .mib_state.instances[0].attributes[0].unsigned = 9' \
	"$temporary/command.json" > "$temporary/command-failed.json"
mkdir -p "$temporary/fail-bin"
cat > "$temporary/fail-bin/mv" <<'EOF'
#!/bin/sh
set -eu
for target do :; done
[ "$target" != "$AIROHA_OMCI_FAIL_MV_TARGET" ] || exit 1
exec /bin/mv "$@"
EOF
chmod +x "$temporary/fail-bin/mv"
if env PATH="$temporary/fail-bin:$PATH" \
	AIROHA_OMCI_FAIL_MV_TARGET="$AIROHA_OMCI_ONU3_STATE" \
	sh "$package/files/airoha-omci-apply" record < "$temporary/command-failed.json"; then
	echo 'persistent ONU3-G write failure was accepted' >&2
	exit 1
fi
cmp -s "$temporary/command.json" "$temporary/state/desired.json"
cmp -s "$temporary/command.json" "$AIROHA_OMCI_ONU3_STATE"
export AIROHA_OMCI_PLATFORM="$package/tests/platform-host"

jq '.service_graph.multicast_operations_profiles[0].dynamic_acl = [{
      row_key: 1, ip_version: 4, gem_port_id: 201, vlan_id: 100,
      source: "0.0.0.0", start: "239.1.1.1", stop: "239.1.1.8",
      imputed_bandwidth: 1000000, preview_length: 0,
      preview_repeat_time: 0, preview_repeat_count: 0,
      preview_reset_time: 0
    }]' "$temporary/multicast-bridge.json" > "$temporary/multicast-acl.json"
sh "$package/files/airoha-omci-apply" < "$temporary/multicast-acl.json"
cmp -s "$temporary/multicast-acl.json" "$temporary/state/desired.json"
tc filter show dev lan1 egress chain 2013 pref 65000 | grep -q 'gact action drop'

jq '.service_graph.multicast_subscribers[0].service_packages = [{
      row_key: 1, vlan_id: 100, max_simultaneous_groups: 8,
      max_multicast_bandwidth: 10000000, operations_profile: 1792
    }]' "$temporary/multicast-bridge.json" > "$temporary/multicast-service.json"
sh "$package/files/airoha-omci-apply" < "$temporary/multicast-service.json"
cmp -s "$temporary/multicast-service.json" "$temporary/state/desired.json"

jq '.service_graph.multicast_subscribers[0].allowed_preview_groups = [{
      row_key: 1, ip_version: 4, source: "0.0.0.0",
      destination: "239.1.1.1", ani_vlan: 100, uni_vlan: 100,
      duration_minutes: 10, time_left_minutes: 10
    }]' "$temporary/multicast-bridge.json" > "$temporary/multicast-preview.json"
sh "$package/files/airoha-omci-apply" < "$temporary/multicast-preview.json"
cmp -s "$temporary/multicast-preview.json" "$temporary/state/desired.json"

sh "$package/files/airoha-omci-apply" < "$package/tests/desired-v2.json"

export AIROHA_OMCI_TC_FAILED="$temporary/tc-bridge-failed"
export AIROHA_OMCI_TC="$package/tests/tc-fail-once"
if sh "$package/files/airoha-omci-apply" < "$temporary/bridge.json"; then
	echo 'injected bridge classifier failure was accepted' >&2
	exit 1
fi
cmp -s "$package/tests/desired-v2.json" "$temporary/state/desired.json"
[ -e "$AIROHA_OMCI_TC_FAILED" ]
if ip link show dev omb0100 >/dev/null 2>&1; then
	echo 'failed bridge candidate survived rollback' >&2
	exit 1
fi
for interface in oma0100 omp0100; do
	if ip link show dev "$interface" >/dev/null 2>&1; then
		echo "failed bridge endpoint $interface survived rollback" >&2
		exit 1
	fi
done
for interface in lan1 lan2 pon; do
	if ip -o link show dev "$interface" | grep -q ' master '; then
		echo "$interface remained enslaved after bridge rollback" >&2
		exit 1
	fi
done
tc filter show dev lan1 ingress chain 2010 | grep -q 'mirred'

jq '.service_graph.extended_vlans[0].rules[0].treatment_inner.priority = 8' \
	"$package/tests/desired-v2.json" > "$temporary/unsupported-evto.json"
if sh "$package/files/airoha-omci-apply" < "$temporary/unsupported-evto.json"; then
	echo 'unsupported EVTO copy treatment was accepted' >&2
	exit 1
fi
cmp -s "$package/tests/desired-v2.json" "$temporary/state/desired.json"
[ "$(cat "$temporary/gpon/data_gems")" = '200:2:3 201:2:3' ]
tc filter show dev lan1 ingress chain 2009 | grep -q 'push id 100'
tc filter show dev lan1 ingress chain 2008 | grep -q 'bpf'
tc filter show dev lan1 egress chain 2012 | grep -q 'bpf'
tc filter show dev lan1 egress chain 2011 | grep -q 'pop'

export AIROHA_OMCI_TC_FAILED="$temporary/tc-failed"
export AIROHA_OMCI_TC="$package/tests/tc-fail-once"
if sh "$package/files/airoha-omci-apply" < "$package/tests/desired-v2-next.json"; then
	echo 'injected tc failure was accepted' >&2
	exit 1
fi
cmp -s "$package/tests/desired-v2.json" "$temporary/state/desired.json"
[ -e "$AIROHA_OMCI_TC_FAILED" ]
tc filter show dev lan1 ingress chain 2010 | grep -q 'mark 2708472008'
tc filter show dev lan1 ingress chain 2010 | grep -q 'mark 2708472009'
tc filter show dev pon ingress chain 2010 | grep -q 'handle 0xa17000c8'
tc filter show dev pon ingress chain 2010 | grep -q 'handle 0xa17000c9'
tc filter show dev lan1 ingress chain 2009 | grep -q 'push id 100'
tc filter show dev lan1 egress chain 2011 | grep -q 'pop'
[ "$(cat "$temporary/gpon/data_gems")" = '200:2:3 201:2:3' ]
