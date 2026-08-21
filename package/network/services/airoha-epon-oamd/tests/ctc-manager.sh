#!/bin/sh
set -eu

base="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT INT TERM

cc -std=c11 -Wall -Wextra -Werror \
	-I"$base/../src" "$base/ctc-manager-test.c" \
	-o "$temporary/ctc-manager-test"
"$temporary/ctc-manager-test"

echo "CTC multi-LLID management transaction tests passed"
