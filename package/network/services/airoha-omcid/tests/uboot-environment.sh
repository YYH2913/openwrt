#!/bin/sh

set -eu

root="${1:?XG2010G workspace root is required}"
openwrt="$root/openwrt"
software="$openwrt/package/network/services/airoha-omcid/files/airoha-omci-software"
environment="$root/u-boot/board/airoha/an7581/xg2010g.env"
defconfig="$root/u-boot/configs/xg2010g_defconfig"
recovery="$root/u-boot/net/lwip/httpd_recovery.c"

[ -f "$software" ] && [ -f "$environment" ] && [ -f "$defconfig" ] &&
	[ -f "$recovery" ]

constant() {
	local name="$1"
	sed -n "s/^${name}='\(.*\)'$/\1/p" "$software"
}

environment_value() {
	local name="$1"
	sed -n "s/^${name}=//p" "$environment"
}

assert_equal() {
	local label="$1" expected="$2" actual="$3"
	[ "$expected" = "$actual" ] || {
		printf '%s differs between OpenWrt and U-Boot\n' "$label" >&2
		printf 'OpenWrt: %s\nU-Boot:  %s\n' "$expected" "$actual" >&2
		exit 1
	}
}

assert_equal rootfs_data_max 0x15800000 "$(environment_value rootfs_data_max)"
assert_equal boot_ubi "$(constant BOOT_UBI)" "$(environment_value boot_ubi)"
assert_equal omci_rollback "$(constant OMCI_ROLLBACK)" "$(environment_value omci_rollback)"
assert_equal omci_boot_commit "$(constant OMCI_BOOT_COMMIT)" "$(environment_value omci_boot_commit)"
assert_equal omci_boot_finish "$(constant OMCI_BOOT_FINISH)" "$(environment_value omci_boot_finish)"
assert_equal omci_boot_stage "$(constant OMCI_BOOT_STAGE)" "$(environment_value omci_boot_stage)"
assert_equal omci_boot_guard "$(constant OMCI_BOOT_GUARD)" "$(environment_value omci_boot_guard)"

grep -qx 'CONFIG_ENV_SIZE=0x4000' "$defconfig"
grep -qx 'CONFIG_CMD_UBI_RENAME=y' "$defconfig"

# The 438 MiB UBI partition must retain enough capacity for two future 32 MiB
# FITs, both environment volumes, UBI internal volumes and the configured
# whole-device bad-block reserve. rootfs_data gains 88 MiB over the former
# limit without consuming the software-image rollback budget.
ubi_pebs=$((0x1b600000 / 0x20000))
leb_size=$((0x1f000))
device_pebs=$((0x20000000 / 0x20000))
bad_pebs=$(((device_pebs * 20 + 1023) / 1024))
internal_pebs=4
environment_pebs=2
rootfs_data_pebs=$(((0x15800000 + leb_size - 1) / leb_size))
fit_pebs=$(((0x02000000 + leb_size - 1) / leb_size))
required_pebs=$((bad_pebs + internal_pebs + environment_pebs +
	rootfs_data_pebs + 2 * fit_pebs))
[ "$required_pebs" -le "$ubi_pebs" ] || {
	printf 'XG2010G UBI budget needs %s PEBs, partition has %s\n' \
		"$required_pebs" "$ubi_pebs" >&2
	exit 1
}

grep -Fq 'env_get("rootfs_data_max")' "$recovery"
grep -Fq 'recovery_create_ubi_volume("rootfs_data", size,' "$recovery"

environment_size="$(wc -c < "$environment")"
[ "$environment_size" -lt $((0x4000)) ] || {
	printf 'XG2010G default environment is %s bytes, limit is %s\n' \
		"$environment_size" "$((0x4000))" >&2
	exit 1
}

# Execute the exact U-Boot environment command strings against a persistent
# file-backed environment and UBI name set. Each injected cut exits the boot
# subshell immediately after the selected durable UBI mutation.
temporary="$(mktemp -d)"
trap 'status=$?; rm -rf "$temporary"; exit "$status"' EXIT
trap 'exit 1' INT TERM
sim_env="$temporary/env"
sim_volumes="$temporary/volumes"

omci_rollback="$(environment_value omci_rollback)"
omci_boot_commit="$(environment_value omci_boot_commit)"
omci_boot_finish="$(environment_value omci_boot_finish)"
omci_boot_stage="$(environment_value omci_boot_stage)"
omci_boot_guard="$(environment_value omci_boot_guard)"

sim_reset() {
	active="$1"
	committed="$2"
	pending="$3"
	shift 3

	rm -rf "$sim_env" "$sim_volumes"
	mkdir -p "$sim_env" "$sim_volumes"
	printf '%s\n' "$active" > "$sim_env/omci_active_id"
	printf '%s\n' "$committed" > "$sim_env/omci_committed_id"
	[ -z "$pending" ] || printf '%s\n' "$pending" > "$sim_env/omci_pending"
	for volume in "$@"; do
		: > "$sim_volumes/$volume"
	done
}

sim_load_environment() {
	omci_active_id="$(cat "$sim_env/omci_active_id")"
	omci_committed_id="$(cat "$sim_env/omci_committed_id")"
	omci_pending="$(cat "$sim_env/omci_pending" 2>/dev/null || true)"
}

setenv() {
	name="$1"
	value="${2:-}"
	case "$name" in
		omci_active_id|omci_committed_id|omci_pending) ;;
		*) return 1 ;;
	esac
	eval "$name=\$value"
}

saveenv() {
	for name in omci_active_id omci_committed_id omci_pending; do
		eval "value=\${$name:-}"
		if [ -n "$value" ]; then
			printf '%s\n' "$value" > "$sim_env/$name"
		else
			rm -f "$sim_env/$name"
		fi
	done
}

sim_cut_after() {
	[ "${AIROHA_TEST_CUT_AFTER:-}" != "$1" ] || exit 97
}

ubi() {
	action="$1"
	shift
	case "$action" in
		check)
			[ "$#" -eq 1 ] && [ -e "$sim_volumes/$1" ]
			;;
		rename)
			[ "$#" -eq 2 ] && [ -e "$sim_volumes/$1" ] &&
				[ ! -e "$sim_volumes/$2" ] || return 1
			mv "$sim_volumes/$1" "$sim_volumes/$2"
			sim_cut_after "rename:$1:$2"
			;;
		remove)
			[ "$#" -eq 1 ] && [ -e "$sim_volumes/$1" ] || return 1
			rm -f "$sim_volumes/$1"
			sim_cut_after "remove:$1"
			;;
		*) return 1 ;;
	esac
}

run() {
	for name in "$@"; do
		eval "command=\${$name:-}"
		[ -n "$command" ] || return 1
		eval "$command" || return $?
	done
}

sim_boot() (
	sim_load_environment
	run omci_boot_guard
)

assert_sim_env() {
	name="$1"
	wanted="$2"
	actual="$(cat "$sim_env/$name" 2>/dev/null || true)"
	[ "$actual" = "$wanted" ] || {
		printf 'simulated %s=%s, want %s\n' "$name" "$actual" "$wanted" >&2
		exit 1
	}
}

assert_sim_volumes() {
	wanted="$(printf '%s\n' "$@" | sort)"
	actual="$(find "$sim_volumes" -mindepth 1 -maxdepth 1 -type f \
		-exec basename {} \; | sort)"
	[ "$actual" = "$wanted" ] || {
		printf 'simulated volumes:\n%s\nwant:\n%s\n' "$actual" "$wanted" >&2
		exit 1
	}
}

assert_cut_then_recovers() {
	cut="$1"
	shift
	if AIROHA_TEST_CUT_AFTER="$cut" sim_boot; then
		printf 'simulated power cut %s did not interrupt boot\n' "$cut" >&2
		exit 1
	else
		status=$?
		[ "$status" -eq 97 ] || {
			printf 'simulated power cut %s returned %s\n' "$cut" "$status" >&2
			exit 1
		}
	fi
	sim_boot
	assert_sim_env omci_active_id 0
	assert_sim_env omci_committed_id 0
	assert_sim_env omci_pending ''
	assert_sim_volumes "$@"
}

# A candidate receives exactly one boot. A second uncommitted boot rolls back.
sim_reset 1 0 1 fit omci_fit_old
sim_boot
assert_sim_env omci_pending 2
assert_sim_volumes fit omci_fit_old
sim_boot
assert_sim_env omci_active_id 0
assert_sim_env omci_committed_id 0
assert_sim_env omci_pending ''
assert_sim_volumes fit

# A prepare marker written before the atomic two-volume rename is harmless.
sim_reset 0 0 prepare fit omci_fit_1
sim_boot
assert_sim_env omci_active_id 0
assert_sim_env omci_committed_id 0
assert_sim_env omci_pending ''
assert_sim_volumes fit omci_fit_1

# Once the prepare rename is complete, rollback remains restartable at each
# following durable mutation and restores the original active image.
for cut in rename:fit:omci_fit_failed \
		rename:omci_fit_old:fit remove:omci_fit_failed; do
	sim_reset 0 0 prepare fit omci_fit_old
	assert_cut_then_recovers "$cut" fit
done

# A cut after active_id was persisted but before prepare became pending=1
# still rolls the candidate back to the committed image.
sim_reset 1 0 prepare fit omci_fit_old
sim_boot
assert_sim_env omci_active_id 0
assert_sim_env omci_committed_id 0
assert_sim_env omci_pending ''
assert_sim_volumes fit

# Never clear prepare when neither a bootable fit nor rollback volume exists.
sim_reset 0 0 prepare omci_fit_1
if sim_boot; then
	echo 'corrupt prepare state was accepted without fit or rollback volume' >&2
	exit 1
fi
assert_sim_env omci_active_id 0
assert_sim_env omci_committed_id 0
assert_sim_env omci_pending prepare
assert_sim_volumes omci_fit_1

# Every durable mutation in rollback is restartable on the following boot.
for cut in rename:fit:omci_fit_failed \
		rename:omci_fit_old:fit remove:omci_fit_failed; do
	sim_reset 1 0 2 fit omci_fit_old
	assert_cut_then_recovers "$cut" fit
done

# A commit persisted before pending is cleared remains committed and converges.
sim_reset 1 1 1 fit omci_fit_old
sim_boot
assert_sim_env omci_pending 2
sim_boot
assert_sim_env omci_active_id 1
assert_sim_env omci_committed_id 1
assert_sim_env omci_pending ''
assert_sim_volumes fit omci_fit_old

# Reconcile a committed inactive image after a cut at every volume rename.
for cut in rename:fit:omci_fit_switch rename:omci_fit_0:fit \
		rename:omci_fit_switch:omci_fit_1; do
	sim_reset 1 0 '' fit omci_fit_0
	assert_cut_then_recovers "$cut" fit omci_fit_1
done
