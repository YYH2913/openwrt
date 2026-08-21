#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
program="$openwrt/package/network/services/airoha-omcid/files/99-airoha-pon-mode"
temporary="$(mktemp -d)"
trap 'status=$?; rm -rf "$temporary"; exit "$status"' EXIT INT TERM

mkdir -p "$temporary/bin"
cat > "$temporary/functions.sh" <<'EOF'
board_name() {
	printf '%s\n' "${AIROHA_TEST_BOARD:-axon,xg2010g}"
}

find_mtd_part() {
	printf '%s\n' "$AIROHA_FACTORY_PART"
}
EOF
cat > "$temporary/system.sh" <<'EOF'
macaddr_canonicalize() {
	value="$(printf '%s' "$1" | tr 'A-F' 'a-f')"
	case "$value" in
		[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f])
			[ "$value" != 00:00:00:00:00:00 ] || return 1
			[ "$value" != ff:ff:ff:ff:ff:ff ] || return 1
			printf '%s\n' "$value"
			;;
		*) return 1 ;;
	esac
}
EOF
cat > "$temporary/bin/uci" <<'EOF'
#!/bin/sh
set -eu
[ "${1:-}" = -q ] && shift
case "${1:-}" in
	get)
		case "$2" in
			airoha-gpon.main.pon_mode) printf '%s\n' "${AIROHA_TEST_MODE:-}" ;;
			airoha-gpon.main.serial_number) printf '%s\n' "${AIROHA_TEST_SERIAL:-}" ;;
			airoha-gpon.main.epon_onu_mac) printf '%s\n' "${AIROHA_TEST_EPON_MAC:-}" ;;
			*) exit 1 ;;
		esac
		;;
	batch)
		cat >> "$AIROHA_TEST_UCI_LOG"
		;;
	*) exit 1 ;;
esac
EOF
cat > "$temporary/bin/logger" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$temporary/bin/uci" "$temporary/bin/logger"

run_defaults() {
	: > "$temporary/uci.log"
	env PATH="$temporary/bin:$PATH" \
		AIROHA_FUNCTIONS="$temporary/functions.sh" \
		AIROHA_SYSTEM_FUNCTIONS="$temporary/system.sh" \
		AIROHA_FACTORY_PART="$temporary/factory" \
		AIROHA_TEST_UCI_LOG="$temporary/uci.log" \
		AIROHA_TEST_BOARD="${AIROHA_TEST_BOARD:-axon,xg2010g}" \
		AIROHA_TEST_MODE="${AIROHA_TEST_MODE:-}" \
		AIROHA_TEST_SERIAL="${AIROHA_TEST_SERIAL:-}" \
		AIROHA_TEST_EPON_MAC="${AIROHA_TEST_EPON_MAC:-}" \
		sh "$program"
}

cat > "$temporary/factory" <<'EOF'
admin_password=must-not-be-imported
wan_mac=00:58:28:B8:CC:48
serial_number=XG2010G2414000318
fsan=axon105027a0
EOF
AIROHA_TEST_BOARD=axon,xg2010g-xgspon run_defaults
grep -Fqx "set airoha-gpon.main.pon_mode='xgspon'" "$temporary/uci.log"
grep -Fqx "set airoha-gpon.main.serial_number='AXON105027A0'" "$temporary/uci.log"
grep -Fqx "set airoha-gpon.main.epon_onu_mac='00:58:28:b8:cc:48'" "$temporary/uci.log"
grep -Fqx "commit airoha-gpon" "$temporary/uci.log"
if grep -Fq 'admin_password' "$temporary/uci.log"; then
	echo 'factory administrator credential was imported into PON configuration' >&2
	exit 1
fi

AIROHA_TEST_MODE=gpon \
AIROHA_TEST_SERIAL=HWTC12345678 \
AIROHA_TEST_EPON_MAC=02:11:22:33:44:55 \
	run_defaults
[ ! -s "$temporary/uci.log" ] || {
	echo 'existing PON identity was overwritten' >&2
	exit 1
}

cat > "$temporary/factory" <<'EOF'
wan_mac=not-a-mac
fsan=INVALID-SERIAL
EOF
AIROHA_TEST_MODE= \
AIROHA_TEST_SERIAL= \
AIROHA_TEST_EPON_MAC= \
	run_defaults
grep -Fqx "set airoha-gpon.main.pon_mode='gpon'" "$temporary/uci.log"
if grep -Eq 'serial_number|epon_onu_mac' "$temporary/uci.log"; then
	echo 'invalid factory PON identity was accepted' >&2
	exit 1
fi

echo 'XG2010G factory PON identity seeding tests passed'
