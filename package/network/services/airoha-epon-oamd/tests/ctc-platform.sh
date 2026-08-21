#!/bin/sh
set -eu

base="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
platform="$base/../files/airoha-epon-ctc-platform"
packet_ip="$(command -v ip)"
packet_nft="$(command -v nft)"
packet_ethtool="$(command -v ethtool)"
temporary="$(mktemp -d)"
cleanup() {
	status=$?
	trap - EXIT INT TERM
	if [ "${AIROHA_CTC_KEEP_TEST_STATE:-0}" = 1 ]; then
		printf 'CTC test state: %s\n' "$temporary" >&2
		exit "$status"
	fi
	rm -rf "$temporary"
	exit "$status"
}
trap cleanup EXIT INT TERM

mkdir -p "$temporary/bin" "$temporary/state" "$temporary/sys/br-lan/bridge"
printf '30000\n' > "$temporary/sys/br-lan/bridge/ageing_time"
for port in 1 2 3 4; do
	mkdir -p "$temporary/sys/lan$port"
	ln -s ../../br-lan "$temporary/sys/lan$port/master"
	printf 'on on on\n' > "$temporary/ethtool-lan$port"
	printf 'on 2500 full\n' > "$temporary/settings-lan$port"
	printf '0x1\n' > "$temporary/sys/lan$port/flags"
done
printf '0x0\n' > "$temporary/sys/lan1/flags"

cat > "$temporary/bin/ethtool" <<'EOF'
#!/bin/sh
set -eu
root="${FAKE_ROOT:?}"
case "$1" in
	lan*)
		read -r autoneg speed duplex < "$root/settings-$1"
		printf 'Settings for %s:\nSpeed: %sMb/s\nDuplex: %s\nAuto-negotiation: %s\n' \
			"$1" "$speed" "$(printf '%s' "$duplex" | awk '{ print toupper(substr($0,1,1)) substr($0,2) }')" "$autoneg"
		;;
	-a)
		read -r autoneg rx tx < "$root/ethtool-$2"
		printf 'Pause parameters for %s:\nAutonegotiate: %s\nRX: %s\nTX: %s\n' \
			"$2" "$autoneg" "$rx" "$tx"
		;;
	-A)
		device="$2"; shift 2
		[ "$1" = autoneg ] && autoneg="$2" && shift 2
		[ "$1" = rx ] && rx="$2" && shift 2
		[ "$1" = tx ] && tx="$2" && shift 2
		[ "$#" -eq 0 ]
		printf '%s %s %s\n' "$autoneg" "$rx" "$tx" > "$root/ethtool-$device"
		printf 'ethtool %s %s %s %s\n' "$device" "$autoneg" "$rx" "$tx" >> "$root/log"
		;;
	-s)
		device="$2"; shift 2
		[ "$1" = autoneg ] && autoneg="$2" && shift 2
		if [ "$autoneg" = off ]; then
			[ "$1" = speed ] && speed="$2" && shift 2
			[ "$1" = duplex ] && duplex="$2" && shift 2
		else
			read -r _ speed duplex < "$root/settings-$device"
		fi
		[ "$#" -eq 0 ]
		printf '%s %s %s\n' "$autoneg" "$speed" "$duplex" > \
			"$root/settings-$device"
		printf 'settings %s %s %s %s\n' "$device" "$autoneg" "$speed" \
			"$duplex" >> "$root/log"
		;;
	-r)
		printf 'restart %s\n' "$2" >> "$root/log"
		;;
	*) exit 2 ;;
esac
EOF

cat > "$temporary/bin/ip" <<'EOF'
#!/bin/sh
set -eu
root="${FAKE_ROOT:?}"
[ "$1:$2:$3:$4" = link:set:dev:"$4" ]
device="$4"; state="$5"
case "$state" in
	up) flags=0x1 ;;
	down) flags=0x0 ;;
	*) exit 2 ;;
esac
printf '%s\n' "$flags" > "$root/sys/$device/flags"
printf 'ip %s %s\n' "$device" "$state" >> "$root/log"
EOF

cat > "$temporary/bin/tc" <<'EOF'
#!/bin/sh
set -eu
root="${FAKE_ROOT:?}"
printf 'tc %s\n' "$*" >> "$root/log"
case "$1:$2" in
	qdisc:show)
		device="$4"
		[ -f "$root/clsact-$device" ] && printf 'qdisc clsact ffff: dev %s\n' "$device"
		;;
	qdisc:add)
		device="$4"
		: > "$root/clsact-$device"
		;;
	filter:del)
		device="$4"; hook="$5"; preference="$9"
		rm -f "$root/filter-$device-$hook-$preference"
		;;
	filter:replace)
		device="$4"; hook="$5"; preference="$9"
		if [ -f "$root/fail-always" ] &&
		   grep -q "^$device:$hook$" "$root/fail-always"; then
			exit 1
		fi
		if [ -f "$root/fail-once" ] && grep -q "^$device:$hook$" "$root/fail-once"; then
			rm -f "$root/fail-once"
			exit 1
		fi
		printf '%s\n' "$*" > "$root/filter-$device-$hook-$preference"
		;;
	*) exit 2 ;;
esac
EOF

cat > "$temporary/bin/nft" <<'EOF'
#!/bin/sh
set -eu
root="${FAKE_ROOT:?}"
case "${1:-}:${2:-}:${3:-}" in
	list:table:bridge)
		[ -f "$root/nft-present" ] || exit 1
		;;
	-f:*)
		file="$2"
		if [ -f "$root/fail-nft-always" ]; then
			exit 1
		fi
		if [ -f "$root/fail-nft-once" ]; then
			rm -f "$root/fail-nft-once"
			exit 1
		fi
		if [ -n "${AIROHA_CTC_REAL_NFT_CHECK:-}" ] &&
		   [ ! -f "$root/nft-present" ]; then
			if [ "${AIROHA_CTC_REAL_NFT_USERNS:-0}" = 1 ]; then
				unshare -Urn "$AIROHA_CTC_REAL_NFT_CHECK" -c -f "$file"
			else
				"$AIROHA_CTC_REAL_NFT_CHECK" -c -f "$file"
			fi
		fi
		cp "$file" "$root/nft-rules"
		if grep -q '^add table bridge airoha_ctc$' "$file"; then
			: > "$root/nft-present"
		else
			rm -f "$root/nft-present"
		fi
		;;
	*) exit 2 ;;
esac
EOF

cat > "$temporary/bin/mv" <<'EOF'
#!/bin/sh
set -eu
root="${FAKE_ROOT:?}"
for target do :; done
if [ -f "$root/fail-state-mv" ] &&
   [ "$target" = "$root/state/effective" ]; then
	rm -f "$root/fail-state-mv"
	exit 1
fi
exec /bin/mv "$@"
EOF
chmod +x "$temporary/bin/ethtool" "$temporary/bin/ip" "$temporary/bin/mv" \
	"$temporary/bin/tc" "$temporary/bin/nft"

export FAKE_ROOT="$temporary"
export AIROHA_CTC_STATE_DIR="$temporary/state"
export AIROHA_CTC_SYS_CLASS_NET="$temporary/sys"
export AIROHA_CTC_ETHTOOL="$temporary/bin/ethtool"
export AIROHA_CTC_IP="$temporary/bin/ip"
export AIROHA_CTC_TC="$temporary/bin/tc"
export AIROHA_CTC_NFT="$temporary/bin/nft"
export PATH="$temporary/bin:$PATH"

apply_first() {
	sh "$platform" apply 600 \
		1 1,100000,8192,4096 - 1 0 1 2,0x81000064,0x8100000a=0x810000c8 - \
		- - 1,50000,100000 - - - 3,0x81000064,0x81000014=0x8100012c,0x8100001e=0x8100012c - \
		- - - - - - 4,0x81000064,0x81000064=0x81000064,0x810000c8=0x810000c8 - \
		- - - - - - 0 -
}

apply_second() {
	sh "$platform" apply 900 \
		0 1,200000,16384,4096 - 0 1 1 1,0x81000190 - \
		- - 1,75000,150000 - - - 2,0x81000064,0x81000032=0x810001f4 - \
		- - - - - - 0 - \
		- - - - - - 0 -
}

apply_first
[ "$(cat "$temporary/ethtool-lan1")" = 'off on on' ]
[ "$(cat "$temporary/settings-lan1")" = 'off 100 full' ]
[ "$(cat "$temporary/sys/lan1/flags")" = '0x1' ]
[ "$(sed -n '7p' "$temporary/state/effective")" = - ]
[ "$(sed -n '8p' "$temporary/state/effective")" = \
	'2,0x81000064,0x8100000a=0x810000c8' ]
grep -q '^restart lan1$' "$temporary/log"
[ "$(cat "$temporary/sys/br-lan/bridge/ageing_time")" = 60000 ]
grep -q 'rate 100000kbit.*burst 12288b' \
	"$temporary/filter-lan1-ingress-20120"
grep -q 'rate 50000kbit.*peakrate 100000kbit' \
	"$temporary/filter-lan2-egress-20121"
grep -q 'update @n1_lan2.*ether saddr.*0x012c.*@ll,112,16' \
	"$temporary/nft-rules"
grep -q 'postrouting oifname "lan2".*ether daddr.*map @n1_lan2' \
	"$temporary/nft-rules"
grep -q 'prerouting iifname "lan3".*0x00c8.*accept' \
	"$temporary/nft-rules"
grep -q 'prerouting iifname "lan1".*0x000a.*set 0x00c8' \
	"$temporary/nft-rules"
grep -q 'vlan push protocol 802.1Q id 100 priority 0 dei 0' \
	"$temporary/filter-lan1-ingress-20110"
grep -q 'handle 0x00c70001/0x00ff000f fw.*vlan pop' \
	"$temporary/filter-lan1-egress-20111"
first_state="$(sha256sum "$temporary/state/effective" | cut -d' ' -f1)"

: > "$temporary/fail-state-mv"
if apply_second; then
	echo 'failed CTC state commit was accepted' >&2
	exit 1
fi
[ "$(sha256sum "$temporary/state/effective" | cut -d' ' -f1)" = "$first_state" ]
[ "$(cat "$temporary/settings-lan1")" = 'off 100 full' ]
[ "$(cat "$temporary/sys/lan1/flags")" = '0x1' ]
[ ! -f "$temporary/state/uncertain" ]

: > "$temporary/fail-nft-once"
if apply_second; then
	echo 'injected CTC nft failure was accepted' >&2
	exit 1
fi
[ "$(sha256sum "$temporary/state/effective" | cut -d' ' -f1)" = "$first_state" ]
grep -q 'prerouting iifname "lan1".*0x000a.*set 0x00c8' \
	"$temporary/nft-rules"
[ ! -f "$temporary/state/uncertain" ]

printf 'lan2:egress\n' > "$temporary/fail-once"
if apply_second; then
	echo 'injected CTC platform failure was accepted' >&2
	exit 1
fi
[ "$(sha256sum "$temporary/state/effective" | cut -d' ' -f1)" = "$first_state" ]
[ "$(cat "$temporary/ethtool-lan1")" = 'off on on' ]
[ "$(cat "$temporary/settings-lan1")" = 'off 100 full' ]
[ "$(cat "$temporary/sys/lan1/flags")" = '0x1' ]
[ "$(cat "$temporary/sys/br-lan/bridge/ageing_time")" = 60000 ]
grep -q 'rate 100000kbit.*burst 12288b' \
	"$temporary/filter-lan1-ingress-20120"
grep -q 'rate 50000kbit.*peakrate 100000kbit' \
	"$temporary/filter-lan2-egress-20121"
grep -q 'prerouting iifname "lan1".*0x000a.*set 0x00c8' \
	"$temporary/nft-rules"
[ ! -f "$temporary/state/uncertain" ]

printf 'lan2:egress\n' > "$temporary/fail-always"
set +e
apply_second
status=$?
set -e
[ "$status" -eq 3 ] || {
	echo "CTC platform rollback failure returned $status instead of 3" >&2
	exit 1
}
[ -f "$temporary/state/uncertain" ]
rm -f "$temporary/fail-always"

sh "$platform" recover
[ "$(cat "$temporary/ethtool-lan1")" = 'on on on' ]
[ "$(cat "$temporary/settings-lan1")" = 'on 100 full' ]
[ "$(cat "$temporary/sys/lan1/flags")" = '0x0' ]
[ "$(cat "$temporary/sys/br-lan/bridge/ageing_time")" = 30000 ]
[ ! -f "$temporary/filter-lan1-ingress-20120" ]
[ ! -f "$temporary/filter-lan2-egress-20121" ]
[ ! -f "$temporary/state/effective" ]
[ ! -f "$temporary/state/pause-lan1" ]
[ ! -f "$temporary/state/phy-lan1" ]
[ ! -f "$temporary/state/autoneg-lan1" ]
[ ! -f "$temporary/state/aging" ]
[ ! -f "$temporary/nft-present" ]
[ ! -f "$temporary/filter-lan1-ingress-20110" ]
[ ! -f "$temporary/filter-lan1-egress-20111" ]

classification='1,7,5+0.1.001122334455+2.1.03+3.1.0064+7.1.06+11.1.01bb;2,1,255+12.1.06+15.1.20010db8000000000000000000000001;3,2,255+16.3.20010db8000000000000000000000040+17.4.20010db8000000000000000000000040;4,3,6+14.3.20010db8000000000000000000000001+15.4.fe800000000000000000000000000001;5,4,7+12.1.06+18.1.06+11.1.01bb'
sh "$platform" apply - \
	- - - - - - 0 "$classification"  - - - - - - 0 - \
	- - - - - - 0 -  - - - - - - 0 -
grep -q 'classification_lan1.*ether daddr == 00:11:22:33:44:55' \
	"$temporary/nft-rules"
grep -q 'classification_lan1.*@ll,112,3 == 3.*@ll,116,12 == 100' \
	"$temporary/nft-rules"
grep -q 'classification_lan1.*@ll,216,8 == 6.*@ll,320,16 == 443' \
	"$temporary/nft-rules"
grep -q 'classification_lan1.*meta priority set 7 @ll,112,16 set @ll,112,16 & 0x1fff | 0xa000 return' \
	"$temporary/nft-rules"
grep -q 'classification_lan1.*@ll,128,16 == 0x86dd.*@ll,208,128 == 0x20010db8000000000000000000000001.*meta priority set 1' \
	"$temporary/nft-rules"
grep -q 'classification_lan1.*@ll,336,128 & 0xffffffffffffffff0000000000000000 <= 0x20010db8000000000000000000000000 & 0xffffffffffffffff0000000000000000' \
	"$temporary/nft-rules"
grep -q 'classification_lan1.*@ll,208,128 & 0xffffffffffffffff0000000000000000 >= 0x20010db8000000000000000000000000 & 0xffffffffffffffff0000000000000000' \
	"$temporary/nft-rules"
grep -q 'classification_lan1.*@ll,96,16 { 0x8100, 0x88a8, 0x9100 }.*@ll,128,16 { 0x8100, 0x88a8, 0x9100 }.*@ll,160,16 == 0x0800.*@ll,180,4 == 5.*@ll,352,16 == 443' \
	"$temporary/nft-rules"
grep -q 'classification_lan1.*@ll,160,16 == 0x8864.*@ll,224,16 == 0x0021.*@ll,244,4 == 5.*@ll,416,16 == 443' \
	"$temporary/nft-rules"
grep -q '^add chain bridge airoha_ctc c1r3p35m0$' \
	"$temporary/nft-rules"
grep -q '^add rule bridge airoha_ctc c1r3p35m0 @ll,392,8 < 184 meta mark set meta mark | 0x80000000 return$' \
	"$temporary/nft-rules"
grep -q '^add rule bridge airoha_ctc c1r3p35m0 @ll,392,8 > 184 return$' \
	"$temporary/nft-rules"
grep -q '^add rule bridge airoha_ctc c1r3p35m0 @ll,384,8 < 13 meta mark set meta mark | 0x80000000 return$' \
	"$temporary/nft-rules"
grep -q '^add chain bridge airoha_ctc c1r3p35m1$' \
	"$temporary/nft-rules"
grep -q '^add rule bridge airoha_ctc c1r3p35m1 @ll,264,8 > 0 meta mark set meta mark | 0x80000000 return$' \
	"$temporary/nft-rules"
grep -q '^add rule bridge airoha_ctc c1r3p35e meta mark set meta mark & 0x7fffffff$' \
	"$temporary/nft-rules"
grep -q '^add rule bridge airoha_ctc c1r3p35e jump c1r3p35m0$' \
	"$temporary/nft-rules"
grep -q '^add rule bridge airoha_ctc c1r3p35e meta mark & 0x80000000 == 0 return$' \
	"$temporary/nft-rules"
grep -q 'classification_lan1.*@ll,160,16 == 0x86dd.*jump c1r3p35e' \
	"$temporary/nft-rules"
grep -q 'classification_lan1 meta mark & 0x80000000 != 0 meta mark set meta mark & 0x7fffffff meta priority set 3 @ll,112,16 set @ll,112,16 & 0x1fff | 0xc000 return comment "precedence-4"' \
	"$temporary/nft-rules"
grep -q 'classification_lan1 meta mark & 0x00ff000f != 0x00c70001.*meta priority set @ll,112,3 map { 0 : 0, 1 : 1, 2 : 2, 3 : 3, 4 : 4, 5 : 5, 6 : 6, 7 : 7 }' \
	"$temporary/nft-rules"
grep -q 'classification_lan1.*@ll,160,16 == 0x86dd.*@ll,224,8 == 6.*@ll,512,16 == 443.*meta priority set 4 @ll,112,16 set @ll,112,16 & 0x1fff | 0xe000 return comment "precedence-5"' \
	"$temporary/nft-rules"
if [ "${AIROHA_CTC_PACKET_CHECK:-0}" = 1 ]; then
	unshare -Urn python3 "$base/ctc-classification-packets.py" \
		"$temporary/nft-rules" "$packet_ip" "$packet_nft" "$packet_ethtool"
fi
sh "$platform" recover

matches=; field=0
while [ "$field" -lt 19 ]; do
	matches="$matches+14.1.20010db8000000000000000000000001"
	field=$((field + 1))
done
max_classification=; rule=1; separator=
while [ "$rule" -le 64 ]; do
	max_classification="$max_classification$separator$rule,7,255$matches"
	separator=';'
	rule=$((rule + 1))
done
{
	printf '%s\n' -
	for port in 1 2 3 4; do
		printf '%s\n' - - - - - - 0 "$max_classification"
	done
} | sh "$platform" apply-stdin
[ "$(wc -l < "$temporary/state/effective")" -eq 33 ]
[ "$(wc -c < "$temporary/state/effective")" -gt 180000 ]
grep -q 'classification_lan4.*precedence-64' "$temporary/nft-rules"
sh "$platform" recover

if {
	line=0
	while [ "$line" -lt 32 ]; do printf '%s\n' -; line=$((line + 1)); done
} | sh "$platform" apply-stdin; then
	echo 'truncated CTC stdin state was accepted' >&2
	exit 1
fi
if {
	line=0
	while [ "$line" -lt 34 ]; do printf '%s\n' -; line=$((line + 1)); done
} | sh "$platform" apply-stdin; then
	echo 'oversized CTC stdin state was accepted' >&2
	exit 1
fi

if sh "$platform" apply - \
		- 1,0,1,0 - - - 0 -  - - - - - - 0 - \
		- - - - - - 0 -  - - - - - - 0 -; then
	echo 'invalid zero-rate CTC policy was accepted' >&2
	exit 1
fi

echo "CTC platform commit, rollback and recovery tests passed"
