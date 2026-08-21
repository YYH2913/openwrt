#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
package="$openwrt/package/network/services/airoha-omcid"
temporary="$(mktemp -d)"
trap 'status=$?; rm -rf "$temporary"; exit "$status"' EXIT
trap 'exit 1' INT TERM

mkdir -p "$temporary/state" "$temporary/fail-bin"
export AIROHA_OMCI_STATE_DIR="$temporary/state"
export AIROHA_OMCI_ONU3_STATE="$temporary/persistent/onu3-state.json"
export AIROHA_OMCI_MULTICAST=/bin/true
export AIROHA_OMCI_PLATFORM=/bin/false

printf '{"revision":1}\n' > "$temporary/first.json"
printf '{"revision":2}\n' > "$temporary/second.json"
sh "$package/files/airoha-omci-apply" record < "$temporary/first.json"
cmp -s "$temporary/first.json" "$temporary/state/desired.json"
cmp -s "$temporary/first.json" "$AIROHA_OMCI_ONU3_STATE"

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
	sh "$package/files/airoha-omci-apply" record < "$temporary/second.json"; then
	echo "persistent ONU3-G write failure was accepted" >&2
	exit 1
fi
cmp -s "$temporary/first.json" "$temporary/state/desired.json"
cmp -s "$temporary/first.json" "$AIROHA_OMCI_ONU3_STATE"
