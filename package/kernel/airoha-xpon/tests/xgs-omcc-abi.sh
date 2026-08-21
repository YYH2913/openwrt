#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
omci_dir="${AIROHA_OMCI_DIR:-$openwrt_dir/../airoha-omci}"
binary="${TMPDIR:-/tmp}/xgs-omcc-abi-test.$$"
trap 'rm -f "$binary"' EXIT INT TERM

[ -f "$omci_dir/go.mod" ] || {
	echo "airoha-omci checkout not found at $omci_dir" >&2
	exit 2
}

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
	-o "$binary" "$script_dir/xgs-omcc-abi-test.c"

# shellcheck disable=SC2046
set -- $("$binary")
[ "$#" -eq 14 ] || {
	echo "unexpected C ABI field count: $#" >&2
	exit 2
}

(
	cd "$omci_dir"
	AIROHA_XGS_OMCC_C_ABI_VERSION="$1" \
	AIROHA_XGS_OMCC_C_MAX_CONTENTS="$2" \
	AIROHA_XGS_OMCC_C_CAP_DS_MIC_VERIFIED="$3" \
	AIROHA_XGS_OMCC_C_CAP_US_MIC_SIGNED="$4" \
	AIROHA_XGS_OMCC_C_GET_INFO="$5" \
	AIROHA_XGS_OMCC_C_MAGIC="$6" \
	AIROHA_XGS_OMCC_C_DIRECTION_RX="$7" \
	AIROHA_XGS_OMCC_C_DIRECTION_TX="$8" \
	AIROHA_XGS_OMCC_C_FLAG_MIC_VERIFIED="$9" \
	AIROHA_XGS_OMCC_C_FLAG_TRAILER_STRIPPED="${10}" \
	AIROHA_XGS_OMCC_C_HEADER_SIZE="${11}" \
	AIROHA_XGS_OMCC_C_STRUCT_SIZE="${12}" \
	AIROHA_XGS_OMCC_C_INSTANCE_GENERATION_OFFSET="${13}" \
	AIROHA_XGS_OMCC_C_SESSION_GENERATION_OFFSET="${14}" \
		go test ./internal/transport -run '^TestKernelDeviceABI$' -count=1
)
