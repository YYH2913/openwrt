#!/bin/sh
set -eu

base="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT INT TERM

cc -std=c11 -Wall -Wextra -Werror \
	-I"$base/../src" "$base/ieee-variable-test.c" \
	-o "$temporary/ieee-variable-test"
"$temporary/ieee-variable-test"

echo "IEEE 802.3ah variable response tests passed"
