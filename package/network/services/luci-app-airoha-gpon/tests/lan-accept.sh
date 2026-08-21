#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
program="$openwrt/package/network/services/luci-app-airoha-gpon/root/usr/sbin/airoha-xg2010g-lan-accept"
temporary="$(mktemp -d)"
trap 'status=$?; rm -rf "$temporary"; exit "$status"' EXIT INT TERM

sysfs="$temporary/sys"
net="$sysfs/class/net"
board="$temporary/board_name"
mkdir -p "$temporary/bin" "$net/br-lan"
printf '%s\n' axon,xg2010g-xgspon > "$board"

cat > "$temporary/bin/sample" <<'EOF'
#!/bin/sh
for counter in "$AIROHA_TEST_SYSFS"/class/net/lan*/statistics/*_bytes; do
	value="$(cat "$counter")"
	printf '%s\n' "$((value + 1000))" > "$counter"
done
EOF
chmod +x "$temporary/bin/sample"

index=10
for port in lan1 lan2 lan3 lan4 pon; do
	path="$net/$port"
	mkdir -p "$path/statistics" "$path/phydev"
	printf '%s\n' "$index" > "$path/ifindex"
	printf '%s\n' 1 > "$path/carrier"
	printf '%s\n' up > "$path/operstate"
	printf '%s\n' full > "$path/duplex"
	printf '%s\n' 100 > "$path/statistics/rx_bytes"
	printf '%s\n' 200 > "$path/statistics/tx_bytes"
	printf '0x%08x\n' "$index" > "$path/phydev/phy_id"
	[ "$port" = pon ] || ln -s "$net/br-lan" "$path/master"
	index=$((index + 1))
done
printf '%s\n' 10000 > "$net/lan1/speed"
printf '%s\n' 10000 > "$net/lan2/speed"
printf '%s\n' 2500 > "$net/lan3/speed"
printf '%s\n' 1000 > "$net/lan4/speed"
printf '%s\n' 2500 > "$net/pon/speed"

run_accept() {
	AIROHA_LAN_ACCEPT_SYSFS_ROOT="$sysfs" \
	AIROHA_LAN_ACCEPT_BOARD_FILE="$board" \
	AIROHA_LAN_ACCEPT_SLEEP="$temporary/bin/sample" \
	AIROHA_LAN_ACCEPT_LOCK_FILE="$temporary/accept.lock" \
	AIROHA_LAN_ACCEPT_SAMPLE_SECONDS=1 \
	AIROHA_TEST_SYSFS="$sysfs" \
		sh "$program" "$1"
}

success="$temporary/success"
AIROHA_LAN_ACCEPT_MIN_RX_BYTES=1000 \
AIROHA_LAN_ACCEPT_MIN_TX_BYTES=1000 \
	run_accept "$success" >/dev/null
grep -Fx 'result=passed' "$success/result.env" >/dev/null
grep -Fx 'lan1_speed_mbps=10000' "$success/initial.env" >/dev/null
grep -Fx 'lan2_speed_mbps=10000' "$success/initial.env" >/dev/null
grep -Fx 'lan3_speed_mbps=2500' "$success/initial.env" >/dev/null
grep -Fx 'lan4_speed_mbps=1000' "$success/initial.env" >/dev/null
for port in lan1 lan2 lan3 lan4; do
	grep -Fx "${port}_master=br-lan" "$success/initial.env" >/dev/null
	grep -Fx "${port}_rx_delta=1000" "$success/final.env" >/dev/null
	grep -Fx "${port}_tx_delta=1000" "$success/final.env" >/dev/null
done

printf '%s\n' 5000 > "$net/lan2/speed"
slow="$temporary/slow"
if run_accept "$slow" >/dev/null 2>&1; then
	echo 'a 5G LAN2 link passed the 10G full-rate gate' >&2
	exit 1
fi
grep -Fx 'failure_reason=lan2-below-full-rate' "$slow/result.env" >/dev/null
printf '%s\n' 10000 > "$net/lan2/speed"

printf '%s\n' 10 > "$net/lan4/ifindex"
duplicate="$temporary/duplicate"
if run_accept "$duplicate" >/dev/null 2>&1; then
	echo 'duplicate LAN netdev identity passed acceptance' >&2
	exit 1
fi
grep -Fx 'failure_reason=lan4-duplicate-ifindex' \
	"$duplicate/result.env" >/dev/null
printf '%s\n' 13 > "$net/lan4/ifindex"

printf '%s\n' 0 > "$net/lan3/carrier"
down="$temporary/down"
if run_accept "$down" >/dev/null 2>&1; then
	echo 'down LAN3 link passed acceptance' >&2
	exit 1
fi
grep -Fx 'failure_reason=lan3-link-down' "$down/result.env" >/dev/null

echo 'XG2010G four-LAN on-target acceptance tests passed'
