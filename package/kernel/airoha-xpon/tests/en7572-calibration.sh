#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source_dir="$(dirname -- "$script_dir")"
binary="${TMPDIR:-/tmp}/en7572-calibration-test.$$"
experimental="${TMPDIR:-/tmp}/en7572-bob-a2-as-a0.$$"
trap 'rm -f "$binary" "$experimental"' EXIT INT TERM
gpon="$source_dir/firmware/airoha/xg2010g/en7572-bob.bin"
xgspon="$source_dir/firmware/airoha/xg2010g/en7572-bob-xgspon.bin"
generator="$source_dir/scripts/en7572-a2-as-a0.sh"
driver="$source_dir/src/airoha-en7572.c"
board_dts="$source_dir/../../../target/linux/airoha/dts/an7581-axon-xg2010g-ubi.dts"
package_makefile="$source_dir/Makefile"

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
	-o "$binary" "$script_dir/en7572-calibration-test.c"
"$binary" "$gpon" a0
"$binary" "$xgspon" a2
sh "$generator" "$xgspon" "$experimental"
"$binary" "$experimental" both

# Everything except the table-wide identity fields is an exact A2 copy.
cmp -n 20 "$experimental" "$xgspon" 0 256
cmp -n 4 "$experimental" "$xgspon" 36 292
cmp -n 200 "$experimental" "$xgspon" 56 312
cmp -n 16 "$experimental" "$xgspon" 20 20
cmp -n 16 "$experimental" "$xgspon" 40 40

grep -F 'airoha,calibration-10g-a2-as-a0' "$driver" >/dev/null
grep -F '"experimental-a2-copy:a0"' "$driver" >/dev/null
grep -F 'airoha,calibration-10g-a2-as-a0;' "$board_dts" >/dev/null
grep -F 'en7572-bob-a2-as-a0.bin' "$package_makefile" >/dev/null

[ "$(sha256sum "$gpon" | cut -d' ' -f1)" = \
	508cb5d29925487eaf4ba9169e566107bf91533c4c6629571d549b6819f78233 ]
[ "$(sha256sum "$xgspon" | cut -d' ' -f1)" = \
	17f7c3facb104f8df58940f50b63c6ffedfda832b03aa15e44322dfeff9ea798 ]
