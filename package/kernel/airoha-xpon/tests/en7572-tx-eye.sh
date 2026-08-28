#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source_dir="$(dirname -- "$script_dir")"
binary="${TMPDIR:-/tmp}/en7572-tx-eye-test.$$"
trap 'rm -f "$binary"' EXIT INT TERM
driver="$source_dir/src/airoha-en7572.c"
header="$source_dir/src/airoha-en7572.h"
core="$source_dir/src/airoha-xpon-core.c"

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
	-o "$binary" "$script_dir/en7572-tx-eye-test.c"
"$binary"

grep -F 'ret = en7572_verify_tx_eye(priv, bank, false);' "$driver" >/dev/null
grep -F 'ret = en7572_verify_tx_eye(priv, bank, true);' "$driver" >/dev/null
grep -F 'int airoha_en7572_validate_mode(' "$header" >/dev/null
grep -F 'return airoha_en7572_validate_mode(core->bosa, mode);' \
	"$core" >/dev/null
