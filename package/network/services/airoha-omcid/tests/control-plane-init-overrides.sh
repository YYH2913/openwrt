#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
omci_init="$openwrt/package/network/services/airoha-omcid/files/airoha-omcid.init"
epon_init="$openwrt/package/network/services/airoha-epon-oamd/files/airoha-epon-oamd.init"
rpc="$openwrt/package/network/services/luci-app-airoha-gpon/root/usr/share/rpcd/ucode/airoha-gpon.uc"
temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT INT TERM

(
	loaded=
	config_load() { loaded="$1"; }
	config_get_bool() { eval "$1=0"; }
	config_get() {
		case "$3" in
			serial_number) value=HWTC12345678 ;;
			pon_mode) value=epon-10g-10g ;;
			omci_mode) value=manual ;;
			*) value="${4:-}" ;;
		esac
		eval "$1=\$value"
	}
	procd_open_instance() { :; }
	procd_set_param() { printf 'set:%s\n' "$*" >> "$temporary/omci.log"; }
	procd_append_param() { printf 'append:%s\n' "$*" >> "$temporary/omci.log"; }
	procd_close_instance() { :; }
	ip() { :; }

	. "$omci_init"
	PERSIST_DIR="$temporary/persist"
	ONU3_STATE="$PERSIST_DIR/onu3-state.json"
	RUNTIME_STATE="$PERSIST_DIR/runtime-state.json"
	RESTART_REASON="$PERSIST_DIR/restart-reason"
	export AIROHA_OMCI_ENABLED_OVERRIDE=1
	export AIROHA_OMCI_MODE_OVERRIDE=auto
	export AIROHA_OMCI_PON_MODE_OVERRIDE=gpon
	start_service
)

grep -q '^append:command -pon-mode gpon$' "$temporary/omci.log"
grep -q '^append:command -interface omci$' "$temporary/omci.log"
if grep -q 'procd_set_param file' "$omci_init"; then
	echo 'OMCI init independently watches UCI files outside the PON transaction' >&2
	exit 1
fi

(
	loaded=
	config_load() { loaded="$1"; }
	config_get_bool() { eval "$1=0"; }
	config_get() {
		case "$3" in
			pon_mode) value=gpon ;;
			*) value="${4:-}" ;;
		esac
		eval "$1=\$value"
	}
	procd_open_instance() { printf 'open\n' >> "$temporary/epon.log"; }
	procd_set_param() { printf 'set:%s\n' "$*" >> "$temporary/epon.log"; }
	procd_close_instance() { :; }

	. "$epon_init"
	export AIROHA_EPON_ENABLED_OVERRIDE=1
	export AIROHA_EPON_OAM_ENABLED_OVERRIDE=1
	export AIROHA_EPON_PON_MODE_OVERRIDE=epon-10g-1g
	start_service
)

grep -q '^open$' "$temporary/epon.log"
grep -q '^set:command /usr/sbin/airoha-epon-oamd ' "$temporary/epon.log"
grep -q -- '--pon-mode epon-10g-1g' "$temporary/epon.log"
if grep -q 'procd_set_param file' "$epon_init"; then
	echo 'EPON OAM init independently watches UCI files outside the PON transaction' >&2
	exit 1
fi
if grep -q '/sbin/reload_config' "$rpc"; then
	echo 'LuCI launches an asynchronous second PON configuration pass' >&2
	exit 1
fi
if sed -n '/function write_config/,/let transaction_lock/p' "$rpc" |
	grep -q 'activation_ready'; then
	echo 'LuCI checks XG/XGS readiness before staging the submitted identity' >&2
	exit 1
fi
grep -Fq 'epon_oam.pon_mode == config.pon_mode' "$rpc" || {
	echo 'LuCI accepts EPON OAM status from the wrong line-rate mode' >&2
	exit 1
}

echo 'OMCI and EPON rollback init overrides passed'
