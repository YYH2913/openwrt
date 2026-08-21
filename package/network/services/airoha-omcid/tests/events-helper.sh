#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
helper="$openwrt/package/network/services/airoha-omcid/files/airoha-omci-events"
temporary="$(mktemp -d)"
helper_pid=
trap 'status=$?; [ -z "$helper_pid" ] || kill "$helper_pid" 2>/dev/null || true; rm -rf "$temporary"; exit "$status"' EXIT
trap 'exit 1' INT TERM

mkdir -p "$temporary/gpon/device" "$temporary/net/omci" "$temporary/bosa/device"
printf '%s\n' 1 > "$temporary/net/omci/carrier"
printf '%s\n' '5 O5' > "$temporary/gpon/device/state"
printf '%s\n' 'sequence=7 bip_count=25000 interval_ms=1000' > "$temporary/gpon/device/ber_sample"
printf '%s\n' '6400 33000 2500 10000 10' > "$temporary/bosa/device/optical_diagnostics"
printf '%s\n' 1 > "$temporary/bosa/device/los"
printf '%s\n' '01234567-89ab-cdef-0123-456789abcdef' > "$temporary/boot_id"

AIROHA_OMCI_GPON_ROOT="$temporary/gpon" \
AIROHA_OMCI_BOSA_ROOT="$temporary/bosa" \
AIROHA_OMCI_NET_ROOT="$temporary/net" \
	AIROHA_OMCI_BOOT_ID_PATH="$temporary/boot_id" \
		sh "$helper" > "$temporary/events" &
helper_pid=$!

tries=0
while [ ! -s "$temporary/events" ] && [ "$tries" -lt 50 ]; do
	sleep 0.1
	tries=$((tries + 1))
done
kill "$helper_pid"
wait "$helper_pid" 2>/dev/null || true
helper_pid=

grep -Fx '{"type":"ber-sample","class_id":263,"entity_id":32769,"sequence":7,"bip_count":25000,"interval_ms":1000,"boot_id":"01234567-89ab-cdef-0123-456789abcdef"}' "$temporary/events" >/dev/null
if grep -F '"alarm_bit":2' "$temporary/events" >/dev/null; then
	echo "BOSA LOS was incorrectly emitted as ANI-G SF" >&2
	exit 1
fi
