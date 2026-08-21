#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
package="$openwrt/package/network/services/airoha-omcid"
temporary="$(mktemp -d)"
trap 'status=$?; rm -rf "$temporary"; exit "$status"' EXIT
trap 'exit 1' INT TERM

mkdir -p "$temporary/bin" "$temporary/sys/ubi/ubi0" "$temporary/sys/ubi/ubi0_0" \
	"$temporary/sys/ubi/ubi0_1" "$temporary/sys/mtd/mtd0"
printf '0\n' > "$temporary/sys/ubi/ubi0/mtd_num"
printf 'ubi\n' > "$temporary/sys/mtd/mtd0/name"

cat > "$temporary/bin/uci" <<'EOF'
#!/bin/sh
set -eu
[ "${1:-}" = -q ] && shift
case "${1:-} ${2:-}" in
"get airoha-omci-images.image0") printf 'image\n' ;;
"get airoha-omci-images.image0.valid") printf '1\n' ;;
"get airoha-omci-images.image1.valid") printf '1\n' ;;
"get airoha-omci-images.image0.version") printf 'old\n' ;;
"get airoha-omci-images.image1.version") printf 'new\n' ;;
"get airoha-omci-images.image0.product_code"|\
"get airoha-omci-images.image1.product_code") printf 'XG2010G\n' ;;
"get airoha-omci-images.image0.image_hash"|\
"get airoha-omci-images.image1.image_hash") printf '\n' ;;
set\ *|commit\ *) ;;
*) exit 1 ;;
esac
EOF
cat > "$temporary/bin/fw_printenv" <<'EOF'
#!/bin/sh
set -eu
[ "${1:-}" = -n ] && [ "$#" -eq 2 ] || exit 1
grep -q "^$2=" "$AIROHA_TEST_FW_ENV" || exit 1
sed -n "s/^$2=//p" "$AIROHA_TEST_FW_ENV"
EOF
cat > "$temporary/bin/fw_setenv" <<'EOF'
#!/bin/sh
set -eu
[ "$#" -eq 1 ] || [ "$#" -eq 2 ] || exit 1
temporary_env="$AIROHA_TEST_FW_ENV.new"
grep -v "^$1=" "$AIROHA_TEST_FW_ENV" > "$temporary_env" || true
[ "$#" -eq 1 ] || printf '%s=%s\n' "$1" "$2" >> "$temporary_env"
mv -f "$temporary_env" "$AIROHA_TEST_FW_ENV"
EOF
cat > "$temporary/bin/ubirename" <<'EOF'
#!/bin/sh
set -eu
shift
while [ "$#" -ne 0 ]; do
	[ "$#" -ge 2 ] || exit 2
	old="$1"
	new="$2"
	path=
	for candidate in "$AIROHA_OMCI_SYS_CLASS"/ubi/ubi[0-9]*_[0-9]*; do
		[ -f "$candidate/name" ] || continue
		[ "$(cat "$candidate/name")" = "$old" ] || continue
		path="$candidate/name"
		break
	done
	[ -n "$path" ] || exit 1
	printf '%s\n' "$new" > "$path"
	shift 2
done
EOF
cat > "$temporary/bin/ubirmvol" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$temporary/bin/uci" "$temporary/bin/fw_printenv" \
	"$temporary/bin/fw_setenv" "$temporary/bin/ubirename" "$temporary/bin/ubirmvol"

fw_env="$temporary/fw-env"
restart_reason="$temporary/persistent/restart-reason"
software="$package/files/airoha-omci-software"

reset_slots() {
	printf 'omci_active_id=0\nomci_committed_id=0\n' > "$fw_env"
	printf 'fit\n' > "$temporary/sys/ubi/ubi0_0/name"
	printf 'omci_fit_1\n' > "$temporary/sys/ubi/ubi0_1/name"
	rm -f "$restart_reason"
}

run_software() {
	env PATH="$temporary/bin:$PATH" \
		AIROHA_TEST_FW_ENV="$fw_env" \
		AIROHA_OMCI_SYS_CLASS="$temporary/sys" \
		AIROHA_OMCI_RESTART_REASON="$restart_reason" \
		AIROHA_OMCI_REBOOT=/bin/true AIROHA_OMCI_REBOOT_DELAY=0 \
		sh "$software" "$@"
}

volume_exists() {
	local wanted="$1" path
	for path in "$temporary/sys/ubi"/ubi[0-9]*_[0-9]*/name; do
		[ -f "$path" ] || continue
		[ "$(cat "$path")" = "$wanted" ] && return 0
	done
	return 1
}

env_is() {
	[ "$(sed -n "s/^$1=//p" "$fw_env")" = "$2" ]
}

reset_slots
run_software activate-prepare 0 0
[ ! -e "$restart_reason" ]
run_software activate-commit 0
[ "$(cat "$restart_reason")" = 1 ]

reset_slots
run_software activate-prepare 1 2
env_is omci_active_id 0
env_is omci_pending prepare
volume_exists fit
volume_exists omci_fit_old
! volume_exists omci_fit_1
[ ! -e "$restart_reason" ]
run_software activate-abort 1
! grep -q '^omci_pending=' "$fw_env"
volume_exists fit
volume_exists omci_fit_1
! volume_exists omci_fit_old

reset_slots
run_software activate-prepare 1 0
run_software state > "$temporary/state.json"
! grep -q '^omci_pending=' "$fw_env"
volume_exists fit
volume_exists omci_fit_1
! volume_exists omci_fit_old
grep -q '"entity_id":0.*"active":true' "$temporary/state.json"

reset_slots
run_software commit-prepare 1
run_software commit-commit 1
env_is omci_active_id 0
env_is omci_committed_id 1
! grep -q '^omci_pending=' "$fw_env"
volume_exists fit
volume_exists omci_fit_1

reset_slots
run_software activate-prepare 1 0
run_software activate-commit 1
env_is omci_active_id 1
env_is omci_pending 1
volume_exists fit
volume_exists omci_fit_old
! volume_exists omci_fit_1
[ "$(cat "$restart_reason")" = 1 ]

run_software commit-prepare 1
run_software commit-commit 1
env_is omci_active_id 1
env_is omci_committed_id 1
! grep -q '^omci_pending=' "$fw_env"
volume_exists fit
volume_exists omci_fit_0
! volume_exists omci_fit_old

reset_slots
mkdir "$temporary/invalid-restart-reason"
run_software activate-prepare 1 0
if env PATH="$temporary/bin:$PATH" \
		AIROHA_TEST_FW_ENV="$fw_env" \
		AIROHA_OMCI_SYS_CLASS="$temporary/sys" \
		AIROHA_OMCI_RESTART_REASON="$temporary/invalid-restart-reason" \
		AIROHA_OMCI_REBOOT=/bin/true AIROHA_OMCI_REBOOT_DELAY=0 \
		sh "$software" activate-commit 1 >/dev/null 2>&1; then
	echo "software activation accepted an unwritable restart-reason target" >&2
	exit 1
fi
env_is omci_active_id 0
! grep -q '^omci_pending=' "$fw_env"
volume_exists fit
volume_exists omci_fit_1
! volume_exists omci_fit_old

# A state read installs every recovery command, including the resumable
# no-pending active/committed reconciliation path.
reset_slots
run_software state >/dev/null
for name in rootfs_data_max boot_ubi omci_rollback omci_boot_commit \
		omci_boot_finish omci_boot_stage omci_boot_guard; do
	grep -q "^$name=" "$fw_env"
done
grep -qx 'rootfs_data_max=0x15800000' "$fw_env"
