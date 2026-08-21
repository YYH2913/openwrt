#!/bin/sh
set -eu

base="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT INT TERM

cc -std=c11 -Wall -Wextra -Werror \
	-I"$base/../src" "$base/ctc-transaction-test.c" \
	-o "$temporary/ctc-transaction-test"
"$temporary/ctc-transaction-test"

echo "CTC cross-layer transaction tests passed"
