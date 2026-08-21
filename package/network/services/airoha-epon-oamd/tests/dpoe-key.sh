#!/bin/sh
set -eu

base="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT INT TERM

cc -std=c11 -Wall -Wextra -Werror \
	-I"$base/../src" "$base/dpoe-key-test.c" \
	-o "$temporary/dpoe-key-test"
"$temporary/dpoe-key-test"

echo "DPoE SIA Get/Set, inventory, optics, AES and retry tests passed"
