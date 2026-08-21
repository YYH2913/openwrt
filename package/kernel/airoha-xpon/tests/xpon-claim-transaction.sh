#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
test_source="$script_dir/xpon-claim-transaction-test.c"
binary="$(mktemp "${TMPDIR:-/tmp}/xpon-claim-transaction.XXXXXX")"
trap 'status=$?; rm -f "$binary"; exit "$status"' EXIT INT TERM

cc -std=c11 -Wall -Wextra -Werror -pedantic \
	-I"$script_dir/../src" "$test_source" -o "$binary"
"$binary"
