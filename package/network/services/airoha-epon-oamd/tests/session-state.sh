#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source_dir="$(dirname -- "$script_dir")/src"
binary="${TMPDIR:-/tmp}/airoha-epon-session-state-test.$$"
trap 'rm -f "$binary"' EXIT INT TERM

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
	-I"$source_dir" -o "$binary" "$script_dir/session-state-test.c"
"$binary"
